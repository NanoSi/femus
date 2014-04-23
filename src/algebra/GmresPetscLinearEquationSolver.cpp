/*=========================================================================

  Program: FEMUS
  Module: PetscLinearEquationSolver
  Authors: Eugenio Aulisa, Simone Bnà
 
  Copyright (c) FEMTTU
  All rights reserved. 

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the above copyright notice for more information.

  =========================================================================*/

//----------------------------------------------------------------------------
// includes :
//----------------------------------------------------------------------------
#include "FEMTTUConfig.h"

#ifdef HAVE_PETSC

// Local Includes
#include "PetscMacro.hpp"
#include "GmresPetscLinearEquationSolver.hpp"
#include "PetscPreconditioner.hpp"
#include "PetscVector.hpp"
#include "PetscMatrix.hpp"
#include <iomanip>

namespace femus {

  using namespace std;

  // ==============================================
  // ----------------------- functions ------
  // ==============================================

  void GmresPetscLinearEquationSolver::set_tolerances(const double &rtol, const double &atol,
						      const double &divtol, const unsigned &maxits, const unsigned &index) {

    _rtol   = static_cast<PetscReal>(rtol);
    _abstol = static_cast<PetscReal>(atol);
    _dtol   = static_cast<PetscReal>(divtol);
    _maxits = static_cast<PetscInt>(maxits);

  };

  // ================================================

  clock_t GmresPetscLinearEquationSolver::BuildIndex(){
  
    clock_t SearchTime = 0;
    clock_t start_time = clock();
    _indexai_init = 1;
   
    unsigned IndexaSize=KKoffset[KKIndex.size()-1][_msh->_iproc] - KKoffset[0][_msh->_iproc];
    _indexai.resize(2);
    _indexai[0].resize(IndexaSize);
    _indexai[1].resize(IndexaSize);
    
    unsigned count0=0;
    unsigned count1=0;
    for(int k=0; k < _SolPdeIndex.size(); k++) {
      unsigned indexSol = _SolPdeIndex[k];
      unsigned soltype = _SolType[indexSol];
      for(unsigned inode_mts = _msh->MetisOffset[soltype][_msh->_iproc]; 
	  inode_mts < _msh->MetisOffset[soltype][_msh->_iproc+1]; inode_mts++) {
	int local_mts = inode_mts-_msh->MetisOffset[soltype][_msh->_iproc];
	int idof_kk = KKoffset[k][_msh->_iproc] +local_mts; 
	if((*(*_Bdc)[indexSol])(inode_mts) < 1.9) {
	  _indexai[0][count0] = idof_kk;
	  count0++;
	}
	else{
	  _indexai[1][count1] = idof_kk;
	  count1++;
	}
      }
    } 
    _indexai[0].resize(count0);
    _indexai[1].resize(count1);
  
    std::sort(_indexai[0].begin(), _indexai[0].end());
    std::sort(_indexai[1].begin(), _indexai[1].end());
  
  
  
    //BEGIN Generate std::vector<IS> for GMRES solve by elimination ***********
    _isA.resize(1);
    PetscInt Asize=_indexai[1].size();
    int ierr=ISCreateGeneral(MPI_COMM_WORLD,Asize,&_indexai[1][0], PETSC_USE_POINTER ,&_isA[0]);	   
    CHKERRABORT(MPI_COMM_WORLD,ierr);
    //END Generate std::vector<IS> for GMRES solve by elimination ***********  
    
  
    clock_t end_time=clock();
    SearchTime = (end_time-start_time);
  
    return SearchTime;
  }

  // ================================================

  std::pair< int, double> GmresPetscLinearEquationSolver::solve(const vector <unsigned> &VankaIndex,
								const short unsigned &NSchurVar,
								const bool &Schur, 
								const bool &ksp_clean) {
    
    clock_t SearchTime, AssemblyTime, SolveTime, UpdateTime;
    int its;
    double final_resid;
    PetscErrorCode ierr; 
    
    // ***************** NODE/ELEMENT SEARCH *******************
    clock_t start_time=clock();
    if(_indexai_init==0) BuildIndex();
    SearchTime = clock() - start_time;
    // ***************** END NODE/ELEMENT SEARCH *******************  
    
    if(_DirichletBCsHandlingMode==0) // By penalty
      {
	PetscVector* EPSCp=static_cast<PetscVector*> (_EPSC);  
	Vec EPSC=EPSCp->vec(); 
	PetscVector* RESp=static_cast<PetscVector*> (_RES);  
	Vec RES=RESp->vec();
	PetscMatrix* KKp=static_cast<PetscMatrix*>(_KK); 
	Mat KK=KKp->mat(); 
      
	// ***************** ASSEMBLE matrix to set Dirichlet BCs by penalty *******************
	start_time=clock();
            
	if(ksp_clean){
	  this->clear();
	  // initialize Pmat wiwth penaly diagonal on the Dirichlet Nodes
	  MatDuplicate(KK,MAT_COPY_VALUES,&_Pmat);
	  MatSetOption(_Pmat,MAT_NO_OFF_PROC_ZERO_ROWS,PETSC_TRUE);
	  MatZeroRows(_Pmat,_indexai[0].size(),&_indexai[0][0],1.e40,0,0);
	  _Pmat_is_initialized = true;
	  this->init(KK,_Pmat);
	}
	AssemblyTime = clock()-start_time;      
	// ***************** END ASSEMBLE ******************

	// ***************** SOLVE ******************
	start_time=clock();

	// Solve the linear system
	ierr = KSPSolve(_ksp, RES, EPSC);			CHKERRABORT(MPI_COMM_WORLD,ierr);

	SolveTime = clock()-start_time;
	// ***************** END SOLVE ******************

	// ***************** RES/EPS UPDATE RES ******************
	start_time=clock();

	*_EPS += *_EPSC;

	_RESC->matrix_mult(*_EPSC,*_KK);
	*_RES -= *_RESC;
                  
	// Get the number of iterations required for convergence
	ierr = KSPGetIterationNumber(_ksp, &its);		CHKERRABORT(MPI_COMM_WORLD,ierr);

	// Get the norm of the final residual to return to the user.
	ierr = KSPGetResidualNorm(_ksp, &final_resid); 	CHKERRABORT(MPI_COMM_WORLD,ierr);

	//this->clear();	

	UpdateTime = clock() - start_time;
	// ***************** END RES/EPS UPDATE ******************
      }
    else if(_DirichletBCsHandlingMode==1) { // By elimination

      PetscVector* RESp=static_cast<PetscVector*> (_RES);
      Vec RES=RESp->vec();
      PetscMatrix* KKp=static_cast<PetscMatrix*>(_KK);
      Mat KK=KKp->mat();
              
      // ***************** ASSEMBLE *******************
      clock_t start_time=clock();
 
      PetscInt Asize=_indexai[1].size();

      IS &isA=_isA[0];
      Vec Pr;
      ierr = VecGetSubVector(RES,isA,&Pr);				CHKERRABORT(MPI_COMM_WORLD,ierr);

      Vec Pw;
      ierr = VecDuplicate(Pr,&Pw);			        	CHKERRABORT(MPI_COMM_WORLD,ierr);

      // initialize Pmat
      if(ksp_clean){
	this->clear();
	ierr = MatGetSubMatrix(KK,isA,isA,MAT_INITIAL_MATRIX,&_Pmat); 	CHKERRABORT(MPI_COMM_WORLD,ierr);
	_Pmat_is_initialized = true;
	this->init(_Pmat,_Pmat);
      }

      AssemblyTime = clock()-start_time;
      // ***************** END ASSEMBLE ******************

      // ***************** SOLVE ******************
      start_time=clock();
          
      // Solve the linear system
      ierr = KSPSolve(_ksp,Pr,Pw);					CHKERRABORT(MPI_COMM_WORLD,ierr);

      // Get the number of iterations required for convergence
      ierr = KSPGetIterationNumber(_ksp, &its);			CHKERRABORT(MPI_COMM_WORLD,ierr);

      // Get the norm of the final residual to return to the user.
      ierr = KSPGetResidualNorm(_ksp, &final_resid); 			CHKERRABORT(MPI_COMM_WORLD,ierr);
 
    
      ierr = VecRestoreSubVector(RES,isA,&Pr);				CHKERRABORT(MPI_COMM_WORLD,ierr);
      ierr = VecDestroy(&Pr);					        CHKERRABORT(MPI_COMM_WORLD,ierr);
        
      SolveTime = clock()-start_time;
      // ***************** END SOLVE ******************

      // ***************** RES/EPS UPDATE ******************
      start_time=clock();

      _EPSC->zero();
      PetscVector* EPSCp=static_cast<PetscVector*> (_EPSC);
      Vec EPSC = EPSCp->vec();

      const PetscInt *ind;
      ierr = ISGetIndices(isA,&ind);				        CHKERRABORT(MPI_COMM_WORLD,ierr);
      PetscScalar *W[1];
      ierr = VecGetArray(Pw,W);						CHKERRABORT(MPI_COMM_WORLD,ierr);
      ierr = VecSetValues(EPSC,Asize,ind,W[0],ADD_VALUES);	        CHKERRABORT(MPI_COMM_WORLD,ierr);
      ierr = VecRestoreArray(Pw,W);				        CHKERRABORT(MPI_COMM_WORLD,ierr);
      ierr = ISRestoreIndices(isA,&ind);					CHKERRABORT(MPI_COMM_WORLD,ierr);

      ierr = VecDestroy(&Pw);					        CHKERRABORT(MPI_COMM_WORLD,ierr);
    
      *_EPS += *_EPSC;

      _RESC->matrix_mult(*_EPSC,*_KK);
      *_RES -= *_RESC;

      _RES->close();
      _EPS->close();

      UpdateTime = clock() - start_time;
      // ***************** END RES/EPS UPDATE ******************
    }
  
    // *** Computational info ***
#ifndef NDEBUG  
    cout << "GMRES Grid: " << _msh->GetGridNumber()<< "      SOLVER TIME:        "  << std::setw(11) << std::setprecision(6) << std::fixed <<
      static_cast<double>( SearchTime + AssemblyTime + SolveTime + UpdateTime)/ CLOCKS_PER_SEC<<
      "  ITS: " << its  << "\t ksp_clean = "<< ksp_clean<<endl;
#endif

    return std::make_pair(its,final_resid);
   
  }

  // ================================================

  void GmresPetscLinearEquationSolver::clear() {
  
    if(_Pmat_is_initialized){
      _Pmat_is_initialized = false;
      MatDestroy(&_Pmat);
    }
    
    if (this->initialized()) {
      this->_is_initialized = false;
      int ierr=0;
      ierr = KSPDestroy(&_ksp);	CHKERRABORT(MPI_COMM_WORLD,ierr);
    }
  }

  // ================================================

  void GmresPetscLinearEquationSolver::init(Mat& Amat, Mat& Pmat) {
  
    // Initialize the data structures if not done so already.
    if (!this->initialized())    {
      this->_is_initialized = true;
      int ierr=0;
      // Create the linear solver context
      ierr = KSPCreate(MPI_COMM_WORLD, &_ksp);					CHKERRABORT(MPI_COMM_WORLD,ierr);
    
      // Create the preconditioner context
      ierr = KSPGetPC(_ksp, &_pc);						CHKERRABORT(MPI_COMM_WORLD,ierr);
   
      // Set user-specified  solver and preconditioner types
      this->set_petsc_solver_type();
    
    
      ierr = KSPSetOperators(_ksp, Amat, Pmat, SAME_PRECONDITIONER);		CHKERRABORT(MPI_COMM_WORLD,ierr);
   

      // Set the tolerances for the iterative solver.  Use the user-supplied
      // tolerance for the relative residual & leave the others at default values.
      ierr = KSPSetTolerances(_ksp,_rtol,_abstol,_dtol,_maxits);	CHKERRABORT(MPI_COMM_WORLD,ierr);
    
      if(_msh->GetGridNumber()!=0)
	KSPSetInitialGuessKnoll(_ksp, PETSC_TRUE);

      if(_msh->GetGridNumber()!=0)
	KSPSetNormType(_ksp,KSP_NORM_NONE);
     
      // Set the options from user-input
      // Set runtime options, e.g., -ksp_type <type> -pc_type <type> -ksp_monitor -ksp_rtol <rtol>
      //  These options will override those specified above as long as
      //  KSPSetFromOptions() is called _after_ any other customization  routines.
      ierr = KSPSetFromOptions(_ksp);						CHKERRABORT(MPI_COMM_WORLD,ierr);
   
      // Notify PETSc of location to store residual history.
      // This needs to be called before any solves, since
      // it sets the residual history length to zero.  The default
      // behavior is for PETSc to allocate (internally) an array
      // of size 1000 to hold the residual norm history.
      ierr = KSPSetResidualHistory(_ksp,
				   PETSC_NULL,   // pointer to the array which holds the history
				   PETSC_DECIDE, // size of the array holding the history
				   PETSC_TRUE);  // Whether or not to reset the history for each solve.
      CHKERRABORT(MPI_COMM_WORLD,ierr);

      PetscPreconditioner::set_petsc_preconditioner_type(this->_preconditioner_type,_pc);
      PetscReal zero = 1.e-16;
      PCFactorSetZeroPivot(_pc,zero);
      PCFactorSetShiftType(_pc,MAT_SHIFT_NONZERO);
    }
  }

  // ================================================

  void GmresPetscLinearEquationSolver::set_petsc_solver_type() {
    int ierr = 0;
    switch (this->_solver_type) {
    case CG:
      ierr = KSPSetType(_ksp, (char*) KSPCG);						CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case CR:
      ierr = KSPSetType(_ksp, (char*) KSPCR);						CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case CGS:
      ierr = KSPSetType(_ksp, (char*) KSPCGS);						CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case BICG:
      ierr = KSPSetType(_ksp, (char*) KSPBICG);					        CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case TCQMR:
      ierr = KSPSetType(_ksp, (char*) KSPTCQMR);					CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case TFQMR:
      ierr = KSPSetType(_ksp, (char*) KSPTFQMR);					CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case LSQR:
      ierr = KSPSetType(_ksp, (char*) KSPLSQR);					        CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case BICGSTAB:
      ierr = KSPSetType(_ksp, (char*) KSPBCGS);					        CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case MINRES:
      ierr = KSPSetType(_ksp, (char*) KSPMINRES);					CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case GMRES:
      ierr = KSPSetType(_ksp, (char*) KSPGMRES);					CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case RICHARDSON:
      ierr = KSPSetType(_ksp, (char*) KSPRICHARDSON);					CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case CHEBYSHEV:
      ierr = KSPSetType(_ksp, (char*) KSPCHEBYSHEV);					CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    case PREONLY:
      ierr = KSPSetType(_ksp, (char*) KSPPREONLY);					CHKERRABORT(MPI_COMM_WORLD,ierr);
      return;
    default:
      std::cerr << "ERROR:  Unsupported PETSC Solver: "
		<< this->_solver_type               << std::endl
		<< "Continuing with PETSC defaults" << std::endl;
    }
  }

} //end namespace femus


#endif 
