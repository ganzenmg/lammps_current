/* ----------------------------------------------------------------------
 *
 *                    *** Smooth Mach Dynamics ***
 *
 * This file is part of the USER-SMD package for LAMMPS.
 * Copyright (2014) Georg C. Ganzenmueller, georg.ganzenmueller@emi.fhg.de
 * Fraunhofer Ernst-Mach Institute for High-Speed Dynamics, EMI,
 * Eckerstrasse 4, D-79104 Freiburg i.Br, Germany.
 *
 * This file is based on fix evaporate from the MISC package.
 *
 * ----------------------------------------------------------------------- */

/* -*- c++ -*- ----------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#ifdef FIX_CLASS

FixStyle(smd/outflow,FixSmdOutflow)

#else

#ifndef LMP_FIX_SMD_INFLOW_H
#define LMP_FIX_OUTFLOW_H

#include "fix.h"

namespace LAMMPS_NS {

class FixSmdOutflow : public Fix {
 public:
  FixSmdOutflow(class LAMMPS *, int, char **);
  ~FixSmdOutflow();
  int setmask();
  void init();
  void pre_exchange();
  double compute_scalar();
  double memory_usage();

 private:
  int nevery,iregion;
  int ndeleted;
  char *idregion;

  int nmax;
  int *list,*mark;
};

}

#endif
#endif

/* ERROR/WARNING messages:

E: Illegal ... command

Self-explanatory.  Check the input script syntax and compare to the
documentation for the command.  You can use -echo screen as a
command-line option when running LAMMPS to see the offending line.

E: Region ID for fix evaporate does not exist

Self-explanatory.

E: Cannot evaporate atoms in atom_modify first group

This is a restriction due to the way atoms are organized in
a list to enable the atom_modify first command.

W: Fix evaporate may delete atom with non-zero molecule ID

This is probably an error, since you should not delete only one atom
of a molecule.

E: Fix evaporate molecule requires atom attribute molecule

The atom style being used does not define a molecule ID.

*/
