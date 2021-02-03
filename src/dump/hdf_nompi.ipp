// Created by Petr Karnakov on 04.07.2020
// Copyright 2020 ETH Zurich

#include <hdf5.h>
#include <mpi.h>
#include <fstream>

#include "hdf.h"
#include "util/logger.h"

template <class M>
template <class Field>
void Hdf<M>::Write(const Field&, std::string, M&, std::string) {
}

template <class M>
template <class Field>
void Hdf<M>::Read(Field&, std::string, M&, std::string) {}

template <class M>
std::vector<size_t> Hdf<M>::GetShape(std::string, std::string) {}

template <class M>
void Hdf<M>::WriteXmf(
    std::string, std::string, const std::array<double, 3>&,
    const std::array<double, 3>&, const std::array<size_t, 3>&, std::string,
    std::string) {}

template <class M>
void Hdf<M>::WriteXmf(
    std::string, std::string, std::string, const M&, std::string) {}