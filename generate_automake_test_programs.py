#!/usr/bin/env python
from __future__ import print_function
from itertools import product

domain_flag = {
        "real"   : "-DTEST_REAL",
        "complex": "-DTEST_COMPLEX",
}
prec_flag = {
        "double" : "-DTEST_DOUBLE",
        "single" : "-DTEST_SINGLE",
}
solver_flag = {
        "1stage" : "-DTEST_SOLVER_1STAGE",
        "2stage" : "-DTEST_SOLVER_2STAGE",
}
gpu_flag = {
        0 : "-DTEST_GPU=0",
        1 : "-DTEST_GPU=1",
}
matrix_flag = {
        "random" : "-DTEST_MATRIX_RANDOM",
        "analytic" : "-DTEST_MATRIX_ANALYTIC",
}

test_type_flag = {
        "eigenvectors" : "-D__EIGENVECTORS",
        "eigenvalues"  : "-D__EIGENVALUES",
        "solve_tridiagonal"  : "-D__SOLVE_TRIDIAGONAL",
}

for m, g, t, p, d, s in product(sorted(matrix_flag.keys()),
                             sorted(gpu_flag.keys()),
                             sorted(test_type_flag.keys()),
                             sorted(prec_flag.keys()),
                             sorted(domain_flag.keys()),
                             sorted(solver_flag.keys())):

    #todo: decide what tests we actually want
    if(m == "analytic" and (g == 1 or t != "eigenvectors" or p == "single" or d == "complex")):
        continue

    if (t == "solve_tridiagonal" and (s == "2stage" or d == "complex")):
        continue

    for kernel in ["all_kernels", "default_kernel"] if s == "2stage" else ["nokernel"]:
        endifs = 0
        extra_flags = []

        if (t == "eigenvalues" and kernel == "all_kernels"):
           continue

        if (g == 1):
            print("if WITH_GPU_VERSION")
            endifs += 1

        if kernel == "default_kernel":
            extra_flags.append("-DTEST_KERNEL=ELPA_2STAGE_{0}_DEFAULT".format(d.upper()))
        elif kernel == "all_kernels":
            extra_flags.append("-DTEST_ALL_KERNELS")

        if (p == "single"):
            if (d == "real"):
                print("if WANT_SINGLE_PRECISION_REAL")
            elif (d == "complex"):
                print("if WANT_SINGLE_PRECISION_COMPLEX")
            else:
                raise Exception("Oh no!")
            endifs += 1

        name = "test_{0}_{1}_{2}_{3}{4}{5}{6}".format(d, p, t, s, "" if kernel == "nokernel" else "_" + kernel, "_gpu" if g else "", "_analytic" if m == "analytic" else "")
        print("noinst_PROGRAMS += " + name)
        print("check_SCRIPTS += " + name + ".sh")
        print(name + "_SOURCES = test/Fortran/test.F90")
        print(name + "_LDADD = $(test_program_ldadd)")
        print(name + "_FCFLAGS = $(test_program_fcflags) \\")
        print("  " + " \\\n  ".join([
            domain_flag[d],
            prec_flag[p],
            test_type_flag[t],
            solver_flag[s],
            gpu_flag[g],
            matrix_flag[m]] + extra_flags))

        print("endif\n" * endifs)
