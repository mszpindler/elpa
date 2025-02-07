//    This file is part of ELPA.
//
//    The ELPA library was originally created by the ELPA consortium,
//    consisting of the following organizations:
//
//    - Max Planck Computing and Data Facility (MPCDF), formerly known as
//      Rechenzentrum Garching der Max-Planck-Gesellschaft (RZG),
//    - Bergische Universität Wuppertal, Lehrstuhl für angewandte
//      Informatik,
//    - Technische Universität München, Lehrstuhl für Informatik mit
//      Schwerpunkt Wissenschaftliches Rechnen ,
//    - Fritz-Haber-Institut, Berlin, Abt. Theorie,
//    - Max-Plack-Institut für Mathematik in den Naturwissenschaften,
//      Leipzig, Abt. Komplexe Strukutren in Biologie und Kognition,
//      and
//    - IBM Deutschland GmbH
//
//    This particular source code file contains additions, changes and
//    enhancements authored by Intel Corporation which is not part of
//    the ELPA consortium.
//
//    More information can be found here:
//    http://elpa.mpcdf.mpg.de/
//
//    ELPA is free software: you can redistribute it and/or modify
//    it under the terms of the version 3 of the license of the
//    GNU Lesser General Public License as published by the Free
//    Software Foundation.
//
//    ELPA is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Lesser General Public License for more details.
//
//    You should have received a copy of the GNU Lesser General Public License
//    along with ELPA.  If not, see <http://www.gnu.org/licenses/>
//
//    ELPA reflects a substantial effort on the part of the original
//    ELPA consortium, and we ask you to respect the spirit of the
//    license that we chose: i.e., please contribute any changes you
//    may have back to the original ELPA library distribution, and keep
//    any derivatives of ELPA under the same license that we chose for
//    the original distribution, the GNU Lesser General Public License.
//
//    This file was ported from the NVIDIA version of the component by A. Poeppl, Intel Corporation

#include "config-f90.h"

#include <CL/sycl.hpp>
#include <stdlib.h>
#include <stdio.h>

#include <complex>
#include <iostream>
#include <cstdint>
#include <vector>
#include <optional>
#include <type_traits>

#include "src/GPU/SYCL/syclCommon.hpp"

// Detect complex number template arguments
namespace {
    template <class> struct is_complex_number                    : public std::false_type {};
    template <class T> struct is_complex_number<std::complex<T>> : public std::true_type {};
}

template<typename T, int wg_size, int sg_size, int step>
inline void reduction_step(T *local_mem, sycl::nd_item<1> &it) {
  auto lId = it.get_local_id(0);

  if constexpr (wg_size >= step && sg_size < step) {
    int constexpr half_step = step >> 1;
    local_mem[lId] += static_cast<T>(lId < half_step) * local_mem[lId + half_step];
    it.barrier(sycl::access::fence_space::local_space);
  }
}

template <typename T, int wg_size, int sg_size>
T parallel_sum_group(sycl::nd_item<1> &it, T *local_mem) {
  reduction_step<T, wg_size, sg_size, 1024>(local_mem, it);
  reduction_step<T, wg_size, sg_size,  512>(local_mem, it);
  reduction_step<T, wg_size, sg_size,  256>(local_mem, it);
  reduction_step<T, wg_size, sg_size,  128>(local_mem, it);
  reduction_step<T, wg_size, sg_size,   64>(local_mem, it);
  reduction_step<T, wg_size, sg_size,   32>(local_mem, it);
  reduction_step<T, wg_size, sg_size,   16>(local_mem, it);
  reduction_step<T, wg_size, sg_size,    8>(local_mem, it);
  reduction_step<T, wg_size, sg_size,    4>(local_mem, it);
  reduction_step<T, wg_size, sg_size,    2>(local_mem, it);

  T local_res = local_mem[it.get_local_id(0)];
  T sg_added_res = sycl::reduce_over_group(it.get_sub_group(), local_res, sycl::plus<>());
  return sycl::group_broadcast(it.get_group(), sg_added_res);
}

template <typename T, int wg_size, int sg_size>
void compute_hh_trafo_c_sycl_kernel(T *q, T const *hh, T const *hh_tau, int const nev, int const nb, int const ldq, int const ncols) {
  // DPC++ & SYCL 1.2.1 is gradually replaced by SYCL2020. This is to keep ELPA compatible with both
#if defined(__INTEL_LLVM_COMPILER) && __INTEL_LLVM_COMPILER < 20230000
  using local_buffer = sycl::accessor<T, 1, sycl::access_mode::read_write, sycl::access::target::local>;
#else
  using local_buffer = sycl::local_accessor<T>;
#endif
  auto device = elpa::gpu::sycl::getDevice();
  auto &queue = elpa::gpu::sycl::getQueue();

  queue.submit([&](sycl::handler &h) {
    local_buffer q_s(sycl::range(nb+1), h);
    local_buffer q_s_reserve(sycl::range(nb), h);

    // For real numbers, using the custom reduction is still a lot faster, for complex ones, the SYCL one is better.
    // And for the custom reduction we need the SLM.
    sycl::range<1> r(0);
    if constexpr (!is_complex_number<T>::value) {
      r = sycl::range<1>(nb + 1);
    }
    local_buffer dotp_s(r, h);

    sycl::range<1> global_range(nev * nb);
    sycl::range<1> local_range(nb);
    h.parallel_for(sycl::nd_range<1>(global_range, local_range), [=](sycl::nd_item<1> it) [[sycl::reqd_sub_group_size(sg_size)]]{
      using sf = sycl::access::fence_space;
      int tid = it.get_local_id(0);
      int local_range = it.get_local_range(0);
      auto g = it.get_group();

      int j = ncols;
      int reserve_counter = wg_size + 2;
      int q_off = (j + tid - 1) * ldq + it.get_group(0);
      int q_off_res = (j - 1) * ldq + it.get_group(0);
      int h_off = tid + (j - 1) * nb;

      q_s[tid] = q[q_off];

      for (; j >= 1; j--) {
        // We can preload the q values into shared local memoory every X iterations
        // instead of doing it in every iteration. This seems to save some time.
        if (reserve_counter > sg_size) {
          q_off_res = sycl::group_broadcast(g, q_off);
          if (j - tid >= 1 && tid <= sg_size) {
            q_s_reserve[tid] = q[q_off_res - tid * ldq];
          }
          reserve_counter = 0;
        }

        // All work items use the same value of hh_tau. Be explicit about only loading it once,
        // and broadcast it to the other work items in the group. (Also, in this loop,
        // in continuation of the above, a q value from the reserve is consumed.
        T hh_tau_jm1;
        if (tid == 0) {
            q_s[tid] = q_s_reserve[reserve_counter];
            hh_tau_jm1 = hh_tau[j-1];
        }
        reserve_counter++;

        hh_tau_jm1 = sycl::group_broadcast(it.get_group(), hh_tau_jm1);
        T q_v2 = q_s[tid];
        T hh_h_off = hh[h_off];

        // For Complex numbers, the native SYCL implementation of the reduction is faster than the hand-coded one. But for
        // real numbers, there's still a significant advantage to using the hand-crafted solution. The correct variant is
        // picked at template instantiation time.
        T dotp_res;
        if constexpr (is_complex_number<T>::value) {
          // I don't get it. Is it now faster or slower?!
          // dotp_res = sycl::reduce_over_group(g, q_v2 * std::conj(hh_h_off), sycl::plus<>());
          dotp_s[tid] = q_v2 * std::conj(hh_h_off); //hh_h_off;
          it.barrier(sf::local_space);
          dotp_res = parallel_sum_group<T, wg_size, sg_size>(it, dotp_s.get_pointer());
        } else {
          dotp_s[tid] = q_v2 * hh_h_off;
          it.barrier(sf::local_space);
          dotp_res = parallel_sum_group<T, wg_size, sg_size>(it, dotp_s.get_pointer());
        }

        q_v2 -= dotp_res * hh_tau_jm1 * hh_h_off;
        q_s[tid + 1] = q_v2;

        if ((j == 1) || (tid == it.get_local_range()[0] - 1)) {
           q[q_off] = q_v2;
        }

        q_off -= ldq;
        q_off_res -= ldq;
        h_off -= nb;
      }
    });
  });
  queue.wait_and_throw();
}

template <typename T>
void launch_compute_hh_trafo_c_sycl_kernel(T *q, const T *hh, const T *hh_tau, const int nev, const int nb, const int ldq, const int ncols) {
  int const sg_size = 32;
  switch (nb) {
    case 1024: compute_hh_trafo_c_sycl_kernel<T, 1024, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    case 512:  compute_hh_trafo_c_sycl_kernel<T, 512, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    case 256:  compute_hh_trafo_c_sycl_kernel<T, 256, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    case 128:  compute_hh_trafo_c_sycl_kernel<T, 128, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    case 64:   compute_hh_trafo_c_sycl_kernel<T, 64, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    case 32:   compute_hh_trafo_c_sycl_kernel<T, 32, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    case 16:   compute_hh_trafo_c_sycl_kernel<T, 16, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    case 8:    compute_hh_trafo_c_sycl_kernel<T, 8, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    case 4:    compute_hh_trafo_c_sycl_kernel<T, 4, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    case 2:    compute_hh_trafo_c_sycl_kernel<T, 2, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    case 1:    compute_hh_trafo_c_sycl_kernel<T, 1, sg_size>(q, hh, hh_tau, nev, nb, ldq, ncols); break;
    default:   abort();
  }
}

extern "C" void launch_compute_hh_trafo_c_sycl_kernel_real_double(double *q, const double *hh, const double *hh_tau, const int nev, const int nb, const int ldq, const int ncols) {
  launch_compute_hh_trafo_c_sycl_kernel<double>(q, hh, hh_tau, nev, nb, ldq, ncols);
}

extern "C" void launch_compute_hh_trafo_c_sycl_kernel_real_single(float *q, const float *hh, const float *hh_tau, const int nev, const int nb, const int ldq, const int ncols) {
  launch_compute_hh_trafo_c_sycl_kernel<float>(q, hh, hh_tau, nev, nb, ldq, ncols);
}

extern "C" void launch_compute_hh_trafo_c_sycl_kernel_complex_double(std::complex<double> *q, const std::complex<double> *hh, const std::complex<double> *hh_tau, const int nev, const int nb, const int ldq, const int ncols) {
  launch_compute_hh_trafo_c_sycl_kernel<std::complex<double>>(q, hh, hh_tau, nev, nb, ldq, ncols);
}

extern "C" void launch_compute_hh_trafo_c_sycl_kernel_complex_single(std::complex<float> *q, const std::complex<float> *hh, const std::complex<float> *hh_tau, const int nev, const int nb, const int ldq, const int ncols) {
  launch_compute_hh_trafo_c_sycl_kernel<std::complex<float>>(q, hh, hh_tau, nev, nb, ldq, ncols);
}
