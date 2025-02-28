/*
  Copyright (C) 2015
    Jakub Krajniak (jkrajniak at gmail.com)

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

#include "python.hpp"
#include "NFixedListEntries.hpp"

using namespace espressopp;  //NOLINT

namespace espressopp {
namespace analysis {
namespace NFixedListEntries {

void registerPython() {
  using namespace espressopp::python;  //NOLINT
  class_<NFixedPairListEntries, bases<Observable> >
      ("analysis_NFixedPairListEntries",
       init< shared_ptr<System>, shared_ptr<FixedPairList> >())
      .add_property("value", &NFixedPairListEntries::compute_real);

  class_<NFixedTripleListEntries, bases<Observable> >
      ("analysis_NFixedTripleListEntries",
       init< shared_ptr<System>, shared_ptr<FixedTripleList> >())
      .add_property("value", &NFixedTripleListEntries::compute_real);

  class_<NFixedQuadrupleListEntries, bases<Observable> >
      ("analysis_NFixedQuadrupleListEntries",
       init< shared_ptr<System>, shared_ptr<FixedQuadrupleList> >())
      .add_property("value", &NFixedQuadrupleListEntries::compute_real);

  class_<NExcludeListEntries, bases<Observable> >
      ("analysis_NExcludeListEntries",
       init< shared_ptr<System>, shared_ptr<VerletList> >())
      .add_property("value", &NExcludeListEntries::compute_real);

  class_<NParticlePairScalingEntries, bases<Observable> >
      ("analysis_NParticlePairScalingEntries",
       init< shared_ptr<System>, shared_ptr<esutil::ParticlePairScaling> >())
      .add_property("value", &NParticlePairScalingEntries::compute_real);
}
}  // end namespace NFixedListEntries
}  // end namespace analysis
}  // end namespace espressopp
