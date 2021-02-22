#include <stdint.h>
#include <stdlib.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>

#include <cstdio>

#include "distr/distrbasic.h"
#include "distr/distrsolver.h"
#include "dump/xmf.h"
#include "func/init_u.h"
#include "geom/mesh.h"
#include "geom/rangemulti.h"
#include "linear/linear.h"
#include "parse/curv.h"
#include "parse/vof.h"
#include "solver/approx.h"
#include "solver/approx_eb.h"
#include "solver/approx_eb.ipp"
#include "solver/cond.h"
#include "solver/curv.h"
#include "solver/electro.h"
#include "solver/proj.h"
#include "solver/solver.h"
#include "solver/tracer.h"
#include "solver/vof.h"
#include "util/format.h"
#include "util/hydro.ipp"
#include "util/linear.h"
#include "util/module.h"
#include "util/timer.h"

static constexpr int kScale = 1;

using M = MeshStructured<double, 2>;
using Scal = typename M::Scal;
using Vect = typename M::Vect;
using MIdx = typename M::MIdx;


auto ToMulti(const std::vector<Scal>& v) {
  Multi<Scal> w(v.size());
  w.data() = v;
  return w;
};

void CopyToCanvas(uint32_t* buf, int w, int h) {
  EM_ASM_(
      {
        let data = Module.HEAPU8.slice($0, $0 + $1 * $2 * 4);
        let tctx = g_tmp_canvas.getContext("2d");
        let image = tctx.getImageData(0, 0, $1, $2);
        image.data.set(data);
        tctx.putImageData(image, 0, 0);
      },
      buf, w, h);
}

struct Par {};

class Solver : public KernelMeshPar<M, Par> {
 public:
  using P = KernelMeshPar<M, Par>; // parent
  using Par = typename P::Par;
  static constexpr size_t dim = M::dim;
  using Sem = typename M::Sem;
  using BlockInfoProxy = generic::BlockInfoProxy<dim>;

  Solver(Vars& var_, const BlockInfoProxy& proxy, Par& par)
      : KernelMeshPar<M, Par>(var_, proxy, par)
      , fc_src(m, 0)
      , fc_src2(m, 0)
      , fc_rho(m, 1)
      , fc_mu(m, 0)
      , fc_force(m, Vect(0))
      , ff_bforce(m, 0) {}
  void Run() override;

 protected:
  using P::m;
  using P::par_;
  using P::var;

 private:
  FieldCell<Scal> fc_src;
  FieldCell<Scal> fc_src2;
  FieldCell<Scal> fc_rho;
  FieldCell<Scal> fc_mu;
  FieldCell<Scal> fc_curv;
  FieldCell<Vect> fc_force;
  FieldCell<Scal> fc_src_tracer;
  FieldEmbed<Scal> ff_bforce;
  MapEmbed<BCondAdvection<Scal>> bc_vof;
  MapEmbed<BCondFluid<Vect>> bc_fluid;
  MapEmbed<BCond<Scal>> bc_electro;
  MapEmbed<BCond<Scal>> bc_vort;
  MapCell<std::shared_ptr<CondCellFluid>> mc_velcond;
  std::unique_ptr<Vof<M>> vof;
  std::unique_ptr<Proj<M>> fluid;
  std::unique_ptr<ElectroInterface<M>> electro;
  Scal maxvel;
  typename PartStrMeshM<M>::Par psm_par;
  typename Vof<M>::Par vof_par;
  std::shared_ptr<linear::Solver<M>> linsolver;
  std::unique_ptr<TracerInterface<M>> tracer;
};

struct Canvas {
  Canvas(MIdx size_) : size(size_), buf(size.prod()) {}
  MIdx size;
  std::vector<uint32_t> buf;
};

struct State {
  State(MPI_Comm comm, Vars& var) : distrsolver(comm, var, par) {}
  typename Solver::Par par;
  DistrSolver<M, Solver> distrsolver;
  int step = 0;
  Scal time = 0;
  Scal dt = 0.001;
  bool pause = false;
  bool to_init_field = true;
  bool to_init_solver = true;
  bool render = false;
  std::vector<std::array<MIdx, 2>> lines; // interface lines

  bool to_spawn = false;
  Vect spawn_c;
  Scal spawn_r;
};

std::shared_ptr<State> g_state;
std::shared_ptr<Canvas> g_canvas;
std::string g_extra_config;
Vars g_var;

static Scal Clamp(Scal f, Scal min, Scal max) {
  return f < min ? min : f > max ? max : f;
}

template <class T>
static T Clamp(T v) {
  return v.max(T(0)).min(T(1));
}

static Scal Clamp(Scal f) {
  return Clamp(f, 0, 1);
}

void GetCircle(FieldCell<Scal>& fcu, Vect c, Scal r, const M& m) {
  Vars par;
  par.String.Set("init_vf", "circlels");
  par.Vect.Set("circle_c", c);
  par.Double.Set("circle_r", r);
  par.Int.Set("dim", 2);
  auto func = CreateInitU<M>(par, false);
  func(fcu, m);
}

static MIdx GetCanvasCoords(Vect x, const Canvas& canvas, const M& m) {
  auto scaledsize = canvas.size * kScale;
  MIdx w = MIdx(x * Vect(scaledsize) / m.GetGlobalLength());
  w = w.max(MIdx(0)).min(scaledsize - MIdx(1));
  w[1] = scaledsize[1] - w[1] - 1;
  return w;
}

template <class MEB>
FieldCell<typename MEB::Scal> GetVortScal(
    const FieldCell<typename MEB::Vect>& fcvel,
    const MapEmbed<BCond<typename MEB::Vect>>& me_vel, MEB& eb) {
  using M = typename MEB::M;
  constexpr auto dim = M::dim;
  auto& m = eb.GetMesh();
  using Scal = typename MEB::Scal;
  using Vect = typename MEB::Vect;
  using UEB = UEmbed<M>;

  std::array<FieldCell<Vect>, dim> grad;
  for (size_t d = 0; d < dim; ++d) {
    grad[d].Reinit(m, Vect(0));
    const auto mebc = GetScalarCond(me_vel, d, m);
    const FieldCell<Scal> fcu = GetComponent(fcvel, d);
    const FieldFace<Scal> ffg = UEB::Gradient(fcu, mebc, m);
    grad[d] = UEB::AverageGradient(ffg, m);
  }

  FieldCell<Scal> res(m, 0);
  for (auto c : m.Cells()) {
    res[c] = grad[1][c][0] - grad[0][c][1];
  }
  return res;
}

static void RenderField(
    Canvas& canvas, const FieldCell<Scal>& fcu, const FieldCell<Vect>& fcvel,
    const MapEmbed<BCond<Vect>>& bc_vel, const MapEmbed<BCond<Scal>>& bc_vort,
    const FieldCell<Scal>& fcp, FieldCell<Scal> fc_pot,
    const MapEmbed<BCond<Scal>>& bc_pot, Multi<FieldCell<Scal>> fc_tracer,
    const Multi<MapEmbed<BCond<Scal>>>& bc_tracer, bool interpolate,
    const M& m) {
  auto fcvelbc = fcvel;
  auto fc_vort = GetVortScal(fcvel, bc_vel, m);
  const auto msize = m.GetGlobalSize();
  const Scal visvel = g_var.Double("visvel", 0);
  const Scal visvort = g_var.Double("visvort", 0);
  const Scal visvf = g_var.Double("visvf", 0);
  const Scal vispot = g_var.Double("vispot", 0);
  const Scal vistracer = g_var.Double("vistracer", 0);

  // fill halo cells
  if (visvel) {
    BcApply<Vect>(fcvelbc, bc_vel, m);
  }
  if (visvort) {
    BcApply<Scal>(fc_vort, bc_vort, m);
  }
  if (vispot) {
    BcApply<Scal>(fc_pot, bc_pot, m);
  }
  if (vistracer) {
    BcApply<Scal>(fc_tracer[0], bc_tracer[0], m);
    // BcApply<Scal>(fc_tracer[1], bc_tracer[1], m);
  }

  using Vect3 = generic::Vect<Scal, 3>;
  using MIdx3 = generic::Vect<unsigned char, 3>;
  auto get_color = [&](IdxCell c) -> Vect3 {
    Vect3 q(0);
    if (visvel) {
      q = interp::Bilinear(
          Clamp(visvel * std::abs(fcvelbc[c][0])),
          Clamp(visvel * std::abs(fcvelbc[c][1])), Vect3(1), Vect3(0, 1, 0),
          Vect3(0, 0, 1), Vect3(0, 1, 1));
    }
    if (visvort) {
      const auto vort = fc_vort[c];
      const auto f = Clamp(std::abs(vort * visvort));
      if (vort > 0) {
        q = interp::Linear(f, Vect3(1, 1, 1), Vect3(1, 0.12, 0.35));
      } else {
        q = interp::Linear(f, Vect3(1, 1, 1), Vect3(0, 0.6, 0.87));
      }
    }
    if (vispot) {
      const auto pot = fc_pot[c];
      const auto f = Clamp(std::abs(pot * vispot));
      if (pot > 0) {
        q = interp::Linear(f, Vect3(1, 1, 1), Vect3(1, 0.12, 0.35));
      } else {
        q = interp::Linear(f, Vect3(1, 1, 1), Vect3(0, 0.6, 0.87));
      }
    }
    if (visvf) {
      q = interp::Linear(visvf * fcu[c], Vect3(q), Vect3(0, 0.8, 0.42));
    }
    if (vistracer && !vispot) {
      const auto u0 = fc_tracer[0][c];
      const auto f0 = Clamp(std::abs(u0 * vistracer));
      q = interp::Linear(f0, Vect3(1, 1, 1), Vect3(1, 0.12, 0.35));
    }
    if (vistracer && vispot) {
      const auto ftr = Clamp(std::abs(fc_tracer[0][c] * vistracer));
      q = interp::Linear(ftr, Vect3(q), Vect3(0, 0.8, 0.42));
    }
    return q;
  };
  if (interpolate && m.GetGlobalSize() < canvas.size) {
    for (auto c : m.SuCellsM()) {
      const MIdx w(c);
      const MIdx bs = canvas.size / msize;
      const MIdx start = (w * canvas.size / msize + bs / 2).max(MIdx(0));
      const MIdx end =
          ((w + MIdx(1)) * canvas.size / msize + bs / 2).min(canvas.size);
      const auto dx = m.direction(0);
      const auto dy = m.direction(1);
      const auto q = get_color(c);
      const auto qx = get_color(c + dx);
      const auto qy = get_color(c + dy);
      const auto qyx = get_color(c + dx + dy);
      for (int y = start[1]; y < end[1]; y++) {
        const Scal fy = Scal(y - start[1]) / (end[1] - start[1]);
        for (int x = start[0]; x < end[0]; x++) {
          const Scal fx = Scal(x - start[0]) / (end[0] - start[0]);
          const Vect f(fx, fy);
          auto qb =
              interp::Bilinear(std::abs(fx), std::abs(fy), q, qx, qy, qyx);
          const MIdx3 mq(qb * 255);
          canvas.buf[canvas.size[0] * (canvas.size[1] - y - 1) + x] =
              0xff000000 | (mq[0] << 0) | (mq[1] << 8) | (mq[2] << 16);
        }
      }
    }
  } else {
    for (auto c : m.CellsM()) {
      const MIdx w(c);
      const MIdx start = w * canvas.size / msize;
      const MIdx end = (w + MIdx(1)) * canvas.size / msize;
      auto q = get_color(c);
      const MIdx3 mq(q * 255);
      const uint32_t v =
          0xff000000 | (mq[0] << 0) | (mq[1] << 8) | (mq[2] << 16);
      for (int y = start[1]; y < end[1]; y++) {
        for (int x = start[0]; x < end[0]; x++) {
          canvas.buf[canvas.size[0] * (canvas.size[1] - y - 1) + x] = v;
        }
      }
    }
  }
}

void Solver::Run() {
  auto sem = m.GetSem();
  auto state = g_state;
  auto& s = *state;
  auto update_vof_par = [&](typename Vof<M>::Par& p) {
    p.dim = 2;
    p.sharpen = var.Int["sharpen"];
    p.sharpen_cfl = var.Double["sharpen_cfl"];
    p.coalth = var.Double["coalth"];
    p.recolor_grid = false;
    p.recolor_reduce = false;
    p.recolor = 0;
  };
  if (sem("init_solver") && s.to_init_solver) {
    if (m.IsRoot()) {
      std::cout << "backend=" << var.String["backend"] << std::endl;
    }
    {
      typename Proj<M>::Par p;
      p.conv = Conv::exp;
      linsolver = ULinear<M>::MakeLinearSolver(var, "symm", m);
      FieldCell<Vect> fcvel(m, Vect(0));

      { // electro
        typename ElectroInterface<M>::Conf conf{var, linsolver};
        electro.reset(new Electro<M>(m, m, bc_electro, 0, conf));
      }

      const size_t ntracers = 1;
      Multi<MapEmbed<BCond<Scal>>> bc_tracer(ntracers);

      const auto topvel = var.Double["topvel"];
      for (auto f : m.AllFacesM()) {
        size_t nci;
        if (m.IsBoundary(f, nci)) {
          const bool f_bottom = (f.direction() == 1 && f[1] == 0);
          const bool f_top =
              (f.direction() == 1 && f[1] == m.GetGlobalSize()[1]);
          /*
          const bool f_left = (f.direction() == 0 && f[0] == 0);
          const bool f_right =
              (f.direction() == 0 && f[0] == m.GetGlobalSize()[0]);
              */
          { // fluid
            auto& bc = bc_fluid[f];
            bc.nci = nci;
            bc.type = BCondFluidType::wall;
            if (topvel && f_top) {
              bc.type = BCondFluidType::slipwall;
              bc.velocity = Vect(topvel, 0);
            }
          }
          { // vof
            auto& bc = bc_vof[f];
            bc.nci = nci;
            bc.halo = BCondAdvection<Scal>::Halo::fill;
            bc.fill_vf = 0;
          }
          { // vorticity
            auto& bc = bc_vort[f];
            bc.nci = nci;
            bc.type = BCondType::neumann;
          }
          { // electro
            auto& bc = bc_electro[f];
            bc.nci = nci;
            if (f_top || f_bottom) {
              bc.type = BCondType::dirichlet;
              bc.val = (f_bottom ? 1 : -1);
            } else {
              bc.type = BCondType::neumann;
            }
          }
          { // tracer
            for (size_t i = 0; i < ntracers; ++i) {
              auto& bc = bc_tracer[i][f];
              bc.nci = nci;
              bc.type = BCondType::neumann;
            }
          }
        }
      }

      { // tracer
        fc_src_tracer.Reinit(m, 0);
        typename TracerInterface<M>::Conf conf;
        conf.layers = ntracers;
        conf.density = ToMulti(var.Vect["tracer_density"]);
        conf.diffusion = ToMulti(var.Vect["tracer_diffusion"]);
        conf.diameter = ToMulti(var.Vect["tracer_diameter"]);
        conf.viscosity = ToMulti(var.Vect["tracer_viscosity"]);
        conf.scheme = GetConvSc(var.String["tracer_scheme"]);
        conf.slip.resize(conf.layers);
        conf.fc_src = &fc_src_tracer;
        using SlipType = typename TracerInterface<M>::SlipType;
        for (size_t i = 0; i < conf.layers; ++i) {
          conf.slip[i].type = SlipType::none;
        }
        Multi<FieldCell<Scal>> vfcu(GRange<size_t>(ntracers), m, 0);
        tracer.reset(new Tracer<M>(m, m, vfcu, bc_tracer, 0, conf));
      }

      { // fluid
        const ProjArgs<M> args{fcvel,   bc_fluid,  mc_velcond, &fc_rho,
                               &fc_mu,  &fc_force, &ff_bforce, &fc_src,
                               &fc_src, 0,         s.dt,       linsolver,
                               p};
        fluid.reset(new Proj<M>(m, m, args));
      }
    }
    {
      auto& p = vof_par;
      update_vof_par(p);
      FieldCell<Scal> fcu(m, 0);
      FieldCell<Scal> fccl(m, 0);
      fc_curv.Reinit(m, 0);
      vof.reset(new Vof<M>(
          m, m, fcu, fccl, bc_vof, &fluid->GetVolumeFlux(), &fc_src2, 0, s.dt,
          p));
    }

    auto ps = ParsePar<PartStr<Scal>>()(m.GetCellSize().norminf(), var);
    psm_par = ParsePar<PartStrMeshM<M>>()(ps, var);
  }
  if (sem.Nested("fluid-start")) {
    fluid->StartStep();
  }
  if (sem.Nested("fluid-iter")) {
    fluid->MakeIteration();
  }
  if (sem.Nested("fluid-finish")) {
    fluid->FinishStep();
  }
  if (sem.Nested("vof-start")) {
    vof->StartStep();
  }
  if (sem.Nested("vof-iter")) {
    vof->MakeIteration();
  }
  if (sem.Nested("vof-finish")) {
    vof->FinishStep();
  }
  if (sem.Nested("vof-post")) {
    vof->PostStep();
  }
  if (sem.Nested("electro-step")) {
    electro->Step(fluid->GetTimeStep(), vof->GetField());
  }
  if (sem.Nested("tracer-step")) {
    tracer->Step(fluid->GetTimeStep(), fluid->GetVolumeFlux());
  }
  if (sem("maxvel")) {
    maxvel = 0;
    const auto& ffv = fluid->GetVolumeFlux();
    for (auto f : m.Faces()) {
      maxvel = std::max(maxvel, std::abs(ffv[f] / m.GetArea(f)));
    }
    m.Reduce(&maxvel, Reduction::max);
  }
  if (sem.Nested("curv")) {
    UCurv<M>::CalcCurvPart(vof.get(), psm_par, &fc_curv, m);
  }
  if (sem("flux")) {
    const Scal rho1 = var.Double["rho1"];
    const Scal rho2 = var.Double["rho2"];
    const Scal mu1 = var.Double["mu1"];
    const Scal mu2 = var.Double["mu2"];
    const Scal sigma = var.Double["sigma"];
    const Vect gravity(var.Vect["gravity"]);
    const Scal tracer_diffusion(var.Vect["tracer_diffusion"][0]);

    {
      auto conf = tracer->GetConf();
      conf.diffusion = ToMulti(var.Vect["tracer_diffusion"]);
      tracer->SetConf(conf);
    }

    const Scal h = m.GetCellSize()[0];
    s.dt = var.Double["cfl"] * h / maxvel;
    if (mu1 != 0) {
      s.dt = std::min(s.dt, var.Double["cflvis"] * h * h / mu1);
    }
    if (tracer_diffusion != 0) {
      s.dt = std::min(s.dt, var.Double["cfldiff"] * h * h / tracer_diffusion);
    }
    if (sigma != 0) {
      s.dt = std::min(
          s.dt, var.Double["cflsurf"] * std::sqrt(
                                            std::pow(h, 3) * (rho1 + rho2) /
                                            (4 * M_PI * std::abs(sigma))));
    }

    s.dt = std::min(s.dt, var.Double["dtmax"]);
    fluid->SetTimeStep(s.dt);
    vof->SetTimeStep(s.dt);
    const auto& fcu = vof->GetField();
    for (auto c : m.AllCells()) {
      const Scal u = fcu[c];
      fc_rho[c] = (1 - u) * rho1 + u * rho2;
      fc_mu[c] = (1 - u) * mu1 + u * mu2;
    }
    for (auto f : m.SuFacesM()) {
      auto rho = (fc_rho[f.cm] + fc_rho[f.cp]) * 0.5;
      ff_bforce[f] = rho * gravity.dot(m.GetNormal(f));
    }

    const FieldFace<Scal> ff_sigma(m, var.Double["sigma"]);
    AppendSurfaceTension(m, ff_bforce, vof->GetField(), fc_curv, ff_sigma);

    if (auto* rate = var.Double.Find("growth_rate")) {
      const auto& fcvf = vof->GetField();
      const auto& trvf0 = tracer->GetVolumeFraction()[0];
      for (auto c : m.Cells()) {
        auto src2 = trvf0[c] * (*rate) * fcvf[c];
        fc_src2[c] = src2;
        fc_src_tracer[c] = -src2;
        fc_src[c] = src2;
      }
    }

    {
      auto& mebc = tracer->GetBCondMutable()[0];
      auto& ffcur = electro->GetFaceCurrent();
      const auto rate_top = var.Double["reaction_rate_top"];
      const auto rate_bottom = var.Double["reaction_rate_bottom"];
      for (auto& p : mebc.GetMapFace()) {
        const auto f = m(p.first);
        auto& bc = p.second;
        const bool f_bottom = (f.direction() == 1 && f[1] == 0);
        const bool f_top = (f.direction() == 1 && f[1] == m.GetGlobalSize()[1]);
        if (f_top) {
          bc.val = rate_top * ffcur[f];
        } else if (f_bottom) {
          bc.val = rate_bottom * ffcur[f];
        }
      }
    }
  }
  if (sem("init") && s.to_init_field) {
    vof->AddModifier([&](FieldCell<Scal>& fcu, FieldCell<Scal>&,
                         const M& m) { //
      fcu.Reinit(m, 0);
    });
  }
  if (sem("spawn") && s.to_spawn) {
    vof->AddModifier(
        [&](FieldCell<Scal>& fcu, FieldCell<Scal>& fccl, const M& m) { //
          FieldCell<Scal> fc_add(m);
          GetCircle(fc_add, s.spawn_c, s.spawn_r, m);
          for (auto c : m.Cells()) {
            if (fc_add[c] > fcu[c]) {
              fcu[c] = fc_add[c];
            }
          }
        });
  }
  if (sem()) {
    s.to_init_solver = false;
    s.to_init_field = false;
    s.to_spawn = false;

    if (s.render) {
      auto bc_vel = GetVelCond<M>(bc_fluid);
      RenderField(
          *g_canvas, vof->GetField(), fluid->GetVelocity(), bc_vel, bc_vort,
          fluid->GetPressure(), electro->GetPotential(), bc_electro,
          tracer->GetVolumeFraction(), tracer->GetBCondMutable(),
          var.Int["visinterp"], m);

      // Render interface lines
      if (m.IsRoot()) {
        s.lines.clear();
      }
      auto h = m.GetCellSize();
      const auto& fci = vof->GetMask();
      const auto& fcn = vof->GetNormal();
      const auto& fca = vof->GetAlpha();
      for (auto c : m.Cells()) {
        if (fci[c]) {
          const auto poly =
              Reconst<Scal>::GetCutPoly(m.GetCenter(c), fcn[c], fca[c], h);
          if (poly.size() == 2) {
            s.lines.push_back({
                GetCanvasCoords(poly[0], *g_canvas, m),
                GetCanvasCoords(poly[1], *g_canvas, m),
            });
          }
        }
      }
    }

    if (m.IsRoot()) {
      ++s.step;
      s.time += vof->GetTimeStep();
    }
  }
}

static void main_loop() {
  if (!g_state) {
    return;
  }
  auto state = g_state;
  auto& s = *state;
  if (s.pause) {
    return;
  }
  std::memset(g_canvas->buf.data(), 0, g_canvas->size.prod() * 4);
  SingleTimer timer;

  const int nsteps = g_var.Int["nsteps"];
  for (int i = 0; i < nsteps; ++i) {
    s.render = (i + 1 == nsteps);
    s.distrsolver.Run();
  }

  const Scal tstep = timer.GetSeconds() / nsteps;

  if (s.step % g_var.Int("reportevery", 10) == 0) {
    std::cout << util::Format(
                     "step={:05} t={:.5f} dt={:.4f} g={:.1f} tstep={:.1f}ms",
                     s.step, s.time, s.dt, Vect(g_var.Vect["gravity"]),
                     tstep * 1e3)
              << std::endl;
  }
  CopyToCanvas(g_canvas->buf.data(), g_canvas->size[0], g_canvas->size[1]);
  EM_ASM_({ Draw(); });
}

static std::string GetBaseConfig() {
  return R"EOF(
set string linsolver_symm conjugate
set double hypre_symm_tol 1e-3
set int hypre_symm_maxiter 100
set int hypre_periodic_x 0
set int hypre_periodic_y 0
set double dtmax 0.1
set double rho1 1
set double rho2 1
set double mu1 0.001
set double mu2 0.001
set double sigma 0
set vect gravity 0 -1
set double topvel 0
set int sharpen 0
set int layers 3
set double coalth 1.5
set double sharpen_cfl 0.1
set int coal 1

set int nsteps 1
set int reportevery 10

set double cfl 1
set double cflvis 0.5
set double cflsurf 2

set double visvel 0
set double visvf 1
set double visvort 0
set int visinterp 0

set int part 1
set double part_h 4
set double part_relax 0.5
set int part_np 7
set double part_segcirc 0
set int part_dn 0
set int part_dump_fr 1
set int part_ns 1
set double part_tol 1e-4
set int part_itermax 10
set int part_verb 0
set int dim 2
set int vtkbin 1
set int vtkmerge 1
)EOF";
}

extern "C" {
Scal AddVelocityAngle(Scal add_deg) {
  if (!g_state) {
    return 0;
  }
  auto& var_g = g_var.Vect["gravity"];
  Vect g(var_g);
  Scal deg = std::atan2(g[1], g[0]) * 180. / M_PI;
  deg += add_deg;
  const Scal rad = deg * M_PI / 180.;
  g = Vect(std::cos(rad), std::sin(rad)) * g.norm();
  std::cout << util::Format("g={:.3f} angle={:.1f}", g, deg) << std::endl;
  var_g = g;
  return deg;
}
void Init() {
  if (!g_state) {
    return;
  }
  auto& s = *g_state;
  s.to_init_field = true;
}
void Spawn(float x, float y, float r) {
  if (!g_state) {
    return;
  }
  auto& s = *g_state;
  s.to_spawn = true;
  s.spawn_c = Vect(x, y);
  s.spawn_r = r;
  std::cout << util::Format("spawn c={:.3f} r={:.3f}", s.spawn_c, s.spawn_r)
            << std::endl;
}
int TogglePause() {
  if (!g_state) {
    return 0;
  }
  auto& s = *g_state;
  s.pause = !s.pause;
  return s.pause;
}
void SetCoal(int flag) {
  return;
}
void SetExtraConfig(const char* conf) {
  g_extra_config = conf;
}

void SetRuntimeConfig(const char* s) {
  std::stringstream conf(s);
  Parser(g_var).ParseStream(conf);
}

void SetMesh(int nx) {
  MPI_Comm comm = 0;
  std::stringstream conf;
  conf << GetDefaultConf();
  Subdomains<MIdx> sub(MIdx(nx), MIdx(nx), 1);
  conf << sub.GetConfig();
  conf << GetBaseConfig();
  conf << g_extra_config << '\n';
  Parser(g_var).ParseStream(conf);

  std::shared_ptr<State> new_state;
  new_state = std::make_shared<State>(comm, g_var);

  g_state = new_state;
  std::cout << util::Format("mesh {}", MIdx(nx)) << std::endl;
}
void SetCanvas(int nx, int ny) {
  g_canvas = std::make_shared<Canvas>(MIdx(nx, ny));
  std::cout << util::Format("canvas {}", g_canvas->size) << std::endl;
}
int GetLines(uint16_t* data, int max_size) {
  if (!g_state) {
    return 0;
  }
  auto state = g_state;
  auto& s = *state;
  int i = 0;
  for (auto p : s.lines) {
    if (i + 3 >= max_size) {
      break;
    }
    data[i] = p[0][0];
    data[i + 1] = p[0][1];
    data[i + 2] = p[1][0];
    data[i + 3] = p[1][1];
    i += 4;
  }
  return i;
}
}

int main() {
  FORCE_LINK(distr_local);
  FORCE_LINK(distr_native);

  SetCanvas(512, 512);
  emscripten_set_canvas_element_size(
      "#canvas", g_canvas->size[0] * kScale, g_canvas->size[1] * kScale);
  emscripten_set_main_loop(main_loop, 30, 1);
}
