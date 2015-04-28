/* ----------------------------------------------------------------------
 LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
 http://lammps.sandia.gov, Sandia National Laboratories
 Steve Plimpton, sjplimp@sandia.gov

 Copyright (2003) Sandia Corporation.  Under the terms of Contract
 DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
 certain rights in this software.  This software is distributed under
 the GNU General Public License.

 See the README file in the top-level LAMMPS directory.
 ------------------------------------------------------------------------- */

#include "mpi.h"
#include "string.h"
#include "stdio.h"
#include "fix_smd_tlsph_reference_configuration.h"
#include "atom.h"
#include "comm.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "force.h"
#include "pair.h"
#include "update.h"
#include "modify.h"
#include "memory.h"
#include "error.h"
#include "domain.h"
#include <Eigen/Eigen>
#include "smd_kernels.h"

using namespace Eigen;
using namespace LAMMPS_NS;
using namespace FixConst;
using namespace SMD_Kernels;
using namespace std;
#define DELTA 16384

/* ---------------------------------------------------------------------- */

FixSMD_TLSPH_ReferenceConfiguration::FixSMD_TLSPH_ReferenceConfiguration(LAMMPS *lmp, int narg, char **arg) :
		Fix(lmp, narg, arg) {

	if (atom->map_style == 0)
		error->all(FLERR, "Pair tlsph with partner list requires an atom map, see atom_modify");

	maxpartner = 1;
	npartner = NULL;
	partner = NULL;
	wfd_list = NULL;
	wf_list = NULL;
	grow_arrays(atom->nmax);
	atom->add_callback(0);

	// initialize npartner to 0 so neighbor list creation is OK the 1st time
	int nlocal = atom->nlocal;
	for (int i = 0; i < nlocal; i++) {
		npartner[i] = 0;
	}

	comm_forward = 14;

	//error->one(FLERR, "hurz!");
}

/* ---------------------------------------------------------------------- */

FixSMD_TLSPH_ReferenceConfiguration::~FixSMD_TLSPH_ReferenceConfiguration() {
	// unregister this fix so atom class doesn't invoke it any more

	atom->delete_callback(id, 0);
// delete locally stored arrays

	memory->destroy(npartner);
	memory->destroy(partner);
	memory->destroy(wfd_list);
	memory->destroy(wf_list);
}

/* ---------------------------------------------------------------------- */

int FixSMD_TLSPH_ReferenceConfiguration::setmask() {
	int mask = 0;
	mask |= PRE_EXCHANGE;
	return mask;
}

/* ---------------------------------------------------------------------- */

void FixSMD_TLSPH_ReferenceConfiguration::init() {
	if (atom->tag_enable == 0)
		error->all(FLERR, "Pair style tlsph requires atoms have IDs");
}

/* ---------------------------------------------------------------------- */

void FixSMD_TLSPH_ReferenceConfiguration::pre_exchange() {
	//return;

	//printf("in FixSMD_TLSPH_ReferenceConfiguration::pre_exchange()\n");
	double **defgrad0 = atom->smd_data_9;
	double *radius = atom->radius;
	//double *contact_radius = atom->contact_radius;
	double **x = atom->x;
	double **x0 = atom->x0;
	double *vfrac = atom->vfrac;
	int nlocal = atom->nlocal;
	int i, itmp;
	double J;
	Matrix3d Ftotal;
	Matrix3d C, eye;
	int *mask = atom->mask;
	if (igroup == atom->firstgroup) {
		nlocal = atom->nfirst;
	}

	eye.setIdentity();

	Matrix3d F0;

	int *updateFlag_ptr = (int *) force->pair->extract("smd/tlsph/updateFlag_ptr", itmp);
	if (updateFlag_ptr == NULL) {
		error->one(FLERR,
				"fix FixSMD_TLSPH_ReferenceConfiguration failed to access updateFlag pointer. Check if a pair style exist which calculates this quantity.");
	}

	Matrix3d *Fincr = (Matrix3d *) force->pair->extract("smd/tlsph/Fincr_ptr", itmp);
	if (Fincr == NULL) {
		error->all(FLERR, "FixSMDIntegrateTlsph::updateReferenceConfiguration() failed to access Fincr array");
	}

	int *nn = (int *) force->pair->extract("smd/tlsph/numNeighsRefConfig_ptr", itmp);
	if (nn == NULL) {
		error->all(FLERR, "FixSMDIntegrateTlsph::updateReferenceConfiguration() failed to access numNeighsRefConfig_ptr array");
	}

	// sum all update flag across processors
	int updateFlag;
	MPI_Allreduce(updateFlag_ptr, &updateFlag, 1, MPI_INT, MPI_MAX, world);

	if (updateFlag > 0) {
		if (comm->me == 0) {
			printf("**** updating ref config at step: %ld\n", update->ntimestep);
		}

		for (i = 0; i < nlocal; i++) {

			if (mask[i] & groupbit) {
				// need determinant of old deformation gradient associated with reference configuration
				F0(0, 0) = defgrad0[i][0];
				F0(0, 1) = defgrad0[i][1];
				F0(0, 2) = defgrad0[i][2];
				F0(1, 0) = defgrad0[i][3];
				F0(1, 1) = defgrad0[i][4];
				F0(1, 2) = defgrad0[i][5];
				F0(2, 0) = defgrad0[i][6];
				F0(2, 1) = defgrad0[i][7];
				F0(2, 2) = defgrad0[i][8];

				// re-set x0 coordinates
				x0[i][0] = x[i][0];
				x0[i][1] = x[i][1];
				x0[i][2] = x[i][2];

				// compute current total deformation gradient
				Ftotal = F0 * Fincr[i];	// this is the total deformation gradient: reference deformation times incremental deformation

				// store the current deformation gradient the reference deformation gradient
				defgrad0[i][0] = Ftotal(0, 0);
				defgrad0[i][1] = Ftotal(0, 1);
				defgrad0[i][2] = Ftotal(0, 2);
				defgrad0[i][3] = Ftotal(1, 0);
				defgrad0[i][4] = Ftotal(1, 1);
				defgrad0[i][5] = Ftotal(1, 2);
				defgrad0[i][6] = Ftotal(2, 0);
				defgrad0[i][7] = Ftotal(2, 1);
				defgrad0[i][8] = Ftotal(2, 2);

				/*
				 * Adjust particle volume as the reference configuration is changed.
				 * We safeguard against excessive deformations by limiting the adjustment range
				 * to the intervale J \in [0.9..1.1]
				 */
				J = Fincr[i].determinant();
				J = MAX(J, 0.8);
				J = MIN(J, 1.2);
				vfrac[i] *= J;

				if (nn[i] < 15) {
					radius[i] *= 1.2;
				} else {
					radius[i] *= pow(J, 1.0 / domain->dimension);
				}

			}
		}

		// update of reference config could have changed x0, vfrac, radius
		// communicate these quantities now to ghosts: x0, vfrac, radius
		comm->forward_comm_fix(this);

		setup(0);
	}
}

/* ----------------------------------------------------------------------
 copy partner info from neighbor lists to atom arrays
 so can be migrated or stored with atoms
 ------------------------------------------------------------------------- */

void FixSMD_TLSPH_ReferenceConfiguration::setup(int vflag) {
	int i, j, ii, jj, n, inum, jnum;
	int *ilist, *jlist, *numneigh, **firstneigh;
	int itype, jtype;
	double r, h, wf, wfd;
	Vector3d dx;

	int nlocal = atom->nlocal;
	nmax = atom->nmax;
	grow_arrays(nmax);

// 1st loop over neighbor list
// calculate npartner for each owned atom
// nlocal_neigh = nlocal when neigh list was built, may be smaller than nlocal

	double **x0 = atom->x0;
	double *radius = atom->radius;
	int *type = atom->type;
	tagint *tag = atom->tag;
	NeighList *list = pair->list;
	inum = list->inum;
	ilist = list->ilist;
	numneigh = list->numneigh;
	firstneigh = list->firstneigh;

	// zero npartner for all current atoms

	for (i = 0; i < nlocal; i++)
		npartner[i] = 0;

	for (ii = 0; ii < inum; ii++) {
		i = ilist[ii];
		itype = type[i];
		jlist = firstneigh[i];
		jnum = numneigh[i];

		for (jj = 0; jj < jnum; jj++) {
			j = jlist[jj];
			j &= NEIGHMASK;
			jtype = type[j];

			dx(0) = x0[i][0] - x0[j][0];
			dx(1) = x0[i][1] - x0[j][1];
			dx(2) = x0[i][2] - x0[j][2];
			r = dx.norm();
			h = radius[i] + radius[j];

			if (r <= h) {
				npartner[i]++;
				if (j < nlocal) {
					npartner[j]++;
				}
			}
		}
	}

	maxpartner = 0;
	for (i = 0; i < nlocal; i++)
		maxpartner = MAX(maxpartner, npartner[i]);
	int maxall;
	MPI_Allreduce(&maxpartner, &maxall, 1, MPI_INT, MPI_MAX, world);
	maxpartner = maxall;

	grow_arrays(nmax);

	for (i = 0; i < nlocal; i++) {
		npartner[i] = 0;
		for (jj = 0; jj < maxpartner; jj++) {
			wfd_list[i][jj] = 0.0;
			wf_list[i][jj] = 0.0;
		}
	}

	for (ii = 0; ii < inum; ii++) {
		i = ilist[ii];
		itype = type[i];
		jlist = firstneigh[i];
		jnum = numneigh[i];

		for (jj = 0; jj < jnum; jj++) {
			j = jlist[jj];
			j &= NEIGHMASK;

			dx(0) = x0[i][0] - x0[j][0];
			dx(1) = x0[i][1] - x0[j][1];
			dx(2) = x0[i][2] - x0[j][2];
			r = dx.norm();
			jtype = type[j];
			h = radius[i] + radius[j];

			if (r < h) {
				spiky_kernel_and_derivative(h, r, domain->dimension, wf, wfd);

				partner[i][npartner[i]] = tag[j];
				wfd_list[i][npartner[i]] = wfd;
				wf_list[i][npartner[i]] = wf;
				npartner[i]++;
				if (j < nlocal) {
					partner[j][npartner[j]] = tag[i];
					wfd_list[j][npartner[j]] = wfd;
					wf_list[j][npartner[j]] = wf;
					npartner[j]++;
				}
			}
		}
	}

	// bond statistics
	if (update->ntimestep == 0) {
		n = 0;
		for (i = 0; i < nlocal; i++) {
			n += npartner[i];
		}
		int nall;
		MPI_Allreduce(&n, &nall, 1, MPI_INT, MPI_SUM, world);

		if (comm->me == 0) {
			if (screen) {
				fprintf(screen, "TLSPH neighbors:\n");
				fprintf(screen, "  max # of neighbors/atom = %d\n", maxpartner);
				fprintf(screen, "  total # of neighbors = %d\n", nall);
				fprintf(screen, "  neighbors/atom = %g\n", (double) nall / atom->natoms);
			}
			if (logfile) {
				fprintf(logfile, "TLSPH neighbors:\n");
				fprintf(logfile, "  total # of neighbors = %d\n", nall);
				fprintf(logfile, "  neighbors/atom = %g\n", (double) nall / atom->natoms);
			}
		}
	}

}

/* ----------------------------------------------------------------------
 memory usage of local atom-based arrays
 ------------------------------------------------------------------------- */

double FixSMD_TLSPH_ReferenceConfiguration::memory_usage() {
	int nmax = atom->nmax;
	int bytes = nmax * sizeof(int);
	bytes += nmax * maxpartner * sizeof(tagint); // partner array
	bytes += nmax * maxpartner * sizeof(float); // damage_per_interaction array
	bytes += nmax * sizeof(int); // npartner array
	return bytes;

}

/* ----------------------------------------------------------------------
 allocate local atom-based arrays
 ------------------------------------------------------------------------- */

void FixSMD_TLSPH_ReferenceConfiguration::grow_arrays(int nmax) {
	//printf("in FixSMD_TLSPH_ReferenceConfiguration::grow_arrays\n");
	memory->grow(npartner, nmax, "tlsph_refconfig_neigh:npartner");
	memory->grow(partner, nmax, maxpartner, "tlsph_refconfig_neigh:partner");
	memory->grow(wfd_list, nmax, maxpartner, "tlsph_refconfig_neigh:wfd");
	memory->grow(wf_list, nmax, maxpartner, "tlsph_refconfig_neigh:wf");
}

/* ----------------------------------------------------------------------
 copy values within local atom-based arrays
 ------------------------------------------------------------------------- */

void FixSMD_TLSPH_ReferenceConfiguration::copy_arrays(int i, int j, int delflag) {
	npartner[j] = npartner[i];
	for (int m = 0; m < npartner[j]; m++) {
		partner[j][m] = partner[i][m];
		wfd_list[j][m] = wfd_list[i][m];
		wf_list[j][m] = wf_list[i][m];
	}
}

/* ----------------------------------------------------------------------
 pack values in local atom-based arrays for exchange with another proc
 ------------------------------------------------------------------------- */

int FixSMD_TLSPH_ReferenceConfiguration::pack_exchange(int i, double *buf) {
// NOTE: how do I know comm buf is big enough if extreme # of touching neighs
// Comm::BUFEXTRA may need to be increased

//printf("pack_exchange ...\n");

	int m = 0;
	buf[m++] = npartner[i];
	for (int n = 0; n < npartner[i]; n++) {
		buf[m++] = partner[i][n];
		buf[m++] = wfd_list[i][n];
		buf[m++] = wf_list[i][n];
	}
	return m;

}

/* ----------------------------------------------------------------------
 unpack values in local atom-based arrays from exchange with another proc
 ------------------------------------------------------------------------- */

int FixSMD_TLSPH_ReferenceConfiguration::unpack_exchange(int nlocal, double *buf) {
	if (nlocal == nmax) {
		//printf("nlocal=%d, nmax=%d\n", nlocal, nmax);
		nmax = nmax / DELTA * DELTA;
		nmax += DELTA;
		grow_arrays(nmax);

		error->message(FLERR,
				"in Fixtlsph_refconfigNeighGCG::unpack_exchange: local arrays too small for receiving partner information; growing arrays");
	}
//printf("nlocal=%d, nmax=%d\n", nlocal, nmax);

	int m = 0;
	npartner[nlocal] = static_cast<int>(buf[m++]);
	for (int n = 0; n < npartner[nlocal]; n++) {
		partner[nlocal][n] = static_cast<tagint>(buf[m++]);
		wfd_list[nlocal][n] = static_cast<float>(buf[m++]);
		wf_list[nlocal][n] = static_cast<float>(buf[m++]);
	}
	return m;
}

/* ----------------------------------------------------------------------
 pack values in local atom-based arrays for restart file
 ------------------------------------------------------------------------- */

int FixSMD_TLSPH_ReferenceConfiguration::pack_restart(int i, double *buf) {
	int m = 0;
	buf[m++] = 4 * npartner[i] + 2;
	buf[m++] = npartner[i];
	for (int n = 0; n < npartner[i]; n++) {
		buf[m++] = partner[i][n];
		buf[m++] = wfd_list[i][n];
		buf[m++] = wf_list[i][n];
	}
	return m;
}

/* ----------------------------------------------------------------------
 unpack values from atom->extra array to restart the fix
 ------------------------------------------------------------------------- */

void FixSMD_TLSPH_ReferenceConfiguration::unpack_restart(int nlocal, int nth) {
// ipage = NULL if being called from granular pair style init()

// skip to Nth set of extra values

//	double **extra = atom->extra;
//
//	int m = 0;
//	for (int i = 0; i < nth; i++)
//		m += static_cast<int>(extra[nlocal][m]);
//	m++;
//
//	// allocate new chunks from ipage,dpage for incoming values
//
//	npartner[nlocal] = static_cast<int>(extra[nlocal][m++]);
//	for (int n = 0; n < npartner[nlocal]; n++) {
//		partner[nlocal][n] = static_cast<tagint>(extra[nlocal][m++]);
//	}
}

/* ----------------------------------------------------------------------
 maxsize of any atom's restart data
 ------------------------------------------------------------------------- */

int FixSMD_TLSPH_ReferenceConfiguration::maxsize_restart() {
// maxtouch_all = max # of touching partners across all procs

	int maxtouch_all;
	MPI_Allreduce(&maxpartner, &maxtouch_all, 1, MPI_INT, MPI_MAX, world);
	return 4 * maxtouch_all + 2;
}

/* ----------------------------------------------------------------------
 size of atom nlocal's restart data
 ------------------------------------------------------------------------- */

int FixSMD_TLSPH_ReferenceConfiguration::size_restart(int nlocal) {
	return 4 * npartner[nlocal] + 2;
}

/* ---------------------------------------------------------------------- */

int FixSMD_TLSPH_ReferenceConfiguration::pack_forward_comm(int n, int *list, double *buf, int pbc_flag, int *pbc) {
	int i, j, m;
	double *radius = atom->radius;
	double *vfrac = atom->vfrac;
	double **x0 = atom->x0;
	double **defgrad0 = atom->smd_data_9;

	//printf("FixSMD_TLSPH_ReferenceConfiguration:::pack_forward_comm\n");
	m = 0;
	for (i = 0; i < n; i++) {
		j = list[i];
		buf[m++] = x0[j][0];
		buf[m++] = x0[j][1];
		buf[m++] = x0[j][2];

		buf[m++] = vfrac[j];
		buf[m++] = radius[j];

		buf[m++] = defgrad0[i][0];
		buf[m++] = defgrad0[i][1];
		buf[m++] = defgrad0[i][2];
		buf[m++] = defgrad0[i][3];
		buf[m++] = defgrad0[i][4];
		buf[m++] = defgrad0[i][5];
		buf[m++] = defgrad0[i][6];
		buf[m++] = defgrad0[i][7];
		buf[m++] = defgrad0[i][8];

	}
	return m;
}

/* ---------------------------------------------------------------------- */

void FixSMD_TLSPH_ReferenceConfiguration::unpack_forward_comm(int n, int first, double *buf) {
	int i, m, last;
	double *radius = atom->radius;
	double *vfrac = atom->vfrac;
	double **x0 = atom->x0;
	double **defgrad0 = atom->smd_data_9;

	m = 0;
	last = first + n;
	for (i = first; i < last; i++) {
		x0[i][0] = buf[m++];
		x0[i][1] = buf[m++];
		x0[i][2] = buf[m++];

		vfrac[i] = buf[m++];
		radius[i] = buf[m++];

		defgrad0[i][0] = buf[m++];
		defgrad0[i][1] = buf[m++];
		defgrad0[i][2] = buf[m++];
		defgrad0[i][3] = buf[m++];
		defgrad0[i][4] = buf[m++];
		defgrad0[i][5] = buf[m++];
		defgrad0[i][6] = buf[m++];
		defgrad0[i][7] = buf[m++];
		defgrad0[i][8] = buf[m++];
	}
}
