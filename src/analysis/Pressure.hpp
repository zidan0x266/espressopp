/*
  Copyright (C) 2012,2013
      Max Planck Institute for Polymer Research
  Copyright (C) 2008,2009,2010,2011
      Max-Planck-Institute for Polymer Research & Fraunhofer SCAI
  
  This file is part of ESPResSo++.
  
  ESPResSo++ is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  ESPResSo++ is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>. 
*/

// ESPP_CLASS
#ifndef _ANALYSIS_PRESSURE_HPP
#define _ANALYSIS_PRESSURE_HPP

#include "types.hpp"
#include "Observable.hpp"

namespace espressopp {
  namespace analysis {
    /** Class to compute the pressure. */
    class Pressure : public Observable {
    public:
      Pressure(shared_ptr< System > system) : Observable(system) {}
      ~Pressure() {}
      virtual real compute() const;
      real compute_real() const;

      static void registerPython();
    };
  }
}

#endif
