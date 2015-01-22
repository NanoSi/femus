#ifndef __optloop_h__
#define __optloop_h__

#include "Typedefs.hpp"
#include "RunTimeMap.hpp"
#include "EqnBase.hpp"
#include "TimeLoop.hpp"



namespace femus {


// Forward class
class Files;
class EqnBase;


class OptLoop  : public TimeLoop {

public:


  OptLoop(Files& files_in);
 ~OptLoop();

  void optimization_loop(EquationsMap& e_map_in);

  void init_equation_data(const EqnBase* eqn);
 
  //====data  
    std::vector<NumericVector *> _x_oldopt;  //old optimization step

};

//prototypes that can stay outside of a class
  double ComputeIntegral (const uint Level, const MeshTwo* mesh, const EqnBase* eqn);

  int ElFlagControl(const std::vector<double> el_xm, const MeshTwo* mesh);



} //end namespace femus



#endif