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
#ifndef _INTERACTION_LENNARDJONESFORCECAPPED_HPP
#define _INTERACTION_LENNARDJONESFORCECAPPED_HPP

#include "FixedPairListInteractionTemplate.hpp"
#include "Potential.hpp"

namespace espressopp {
namespace interaction {
    /** This class provides methods to compute forces and energies of
	the Lennard Jones potential with capped energy.

	\f[ V(r) = 4 \varepsilon \left[ \left( \frac{\sigma}{r} \right)^{12} -
	\left( \frac{\sigma}{r} \right)^{6} \right]
	\f]

    */
class LennardJonesForceCapped : public PotentialTemplate< LennardJonesForceCapped > {
 private:
  real epsilon;
  real sigma;
  real ff1, ff2;
  real ef1, ef2;
  real max_force;

 public:
  static void registerPython();

  LennardJonesForceCapped() : epsilon(0.0), sigma(0.0), max_force(-1) {
    setShift(0.0);
    setCutoff(infinity);
    preset();
  }

  LennardJonesForceCapped(real _epsilon, real _sigma,
    real _cutoff, real _max_force, real _shift)
    : epsilon(_epsilon), sigma(_sigma), max_force(_max_force) {

    setShift(_shift);
    setCutoff(_cutoff);
    preset();

  }

  LennardJonesForceCapped(real _epsilon, real _sigma,
    real _cutoff, real _max_force)
    : epsilon(_epsilon), sigma(_sigma), max_force(_max_force) {
    autoShift = false;
    setCutoff(_cutoff);
    preset();
    setAutoShift();

  }

  void preset() {
    real sig2  = sigma * sigma;
    real sig6  = sig2 * sig2 * sig2;
    ff1        = 48.0 * epsilon * sig6 * sig6;
    ff2        = 24.0 * epsilon * sig6;
    ef1        =  4.0 * epsilon * sig6 * sig6;
    ef2        =  4.0 * epsilon * sig6;
  }

  // Setter and getter
  void setEpsilon(real _epsilon) {
    epsilon = _epsilon;
    updateAutoShift();
    preset();
  }

  real getEpsilon() const { return epsilon; }

  void setMaxforce(real _max_force) {
    max_force = _max_force;
  }

  real getMaxforce() const { return max_force; }

  void setSigma(real _sigma) {
    sigma = _sigma;
    updateAutoShift();
    preset();
  }
  real getSigma() const { return sigma; }

  real _computeEnergySqrRaw(real distSqr) const {
      real frac2 = sigma*sigma / distSqr;
      real frac6 = frac2 * frac2 * frac2;
      real energy = 4.0 * epsilon * (frac6 * frac6 - frac6);
      return energy;
  }

  bool _computeForceRaw(Real3D& force,
                          const Real3D& dist,
                          real distSqr) const {
    real frac2 = 1.0 / distSqr;
    real frac6 = frac2 * frac2 * frac2;
    real ffactor = frac6 * (ff1 * frac6 - ff2) * frac2;
    force = dist * ffactor;
    if (max_force != -1) {
      if (force.isNaNInf()) {
        force = (dist / dist.abs()) * max_force;
      } else {
        real abs_force = force.abs();
        if (abs_force > max_force) {
          force = (force / abs_force) * max_force;
        }
      }
    }
    return true;
  }
};
// provide pickle support
struct LennardJonesForceCapped_pickle : boost::python::pickle_suite {
  static
  boost::python::tuple
  getinitargs(LennardJonesForceCapped const& pot) {
    real eps;
    real sig;
    real rc;
    real sh;
    eps = pot.getEpsilon();
    sig = pot.getSigma();
    rc = pot.getCutoff();
    sh = pot.getShift();
    return boost::python::make_tuple(eps, sig, rc, sh);
  }
};

}  // namespace interaction
}  // namespace espressopp

#endif
