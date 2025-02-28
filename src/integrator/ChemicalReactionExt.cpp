/*
   Copyright (C) 2014-2017
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

#include "ChemicalReactionExt.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "storage/Storage.hpp"
#include "iterator/CellListIterator.hpp"
#include "esutil/RNG.hpp"
#include "storage/NodeGrid.hpp"
#include "storage/DomainDecomposition.hpp"
#include "FixDistances.hpp"

namespace espressopp {
namespace integrator {
LOG4ESPP_LOGGER(ChemicalReaction::theLogger, "ChemicalReaction");

/** ChemicalReaction extension to the integrator
 *
 * @param system The espressopp.System object.
 * @param verletList The espressopp.VerletList object.
 * @param domdec The espressopp.storage.DomainDecomposition object.
 */
ChemicalReaction::ChemicalReaction(shared_ptr<System> system, shared_ptr<VerletList> verletList,
    shared_ptr<storage::DomainDecomposition> domdec, shared_ptr<TopologyManager> tm)
      :Extension(system),
          verlet_list_(verletList),
          domdec_(domdec), tm_(tm), is_nearest_(false) {
  type = Extension::Reaction;
  extensionOrder = Extension::withReaction;

  current_cutoff_ = verletList->getVerletCutoff() - system->getSkin();

  if (!system->rng)
    throw std::runtime_error("System has no RNG.");

  rng_ = system->rng;
  LOG4ESPP_INFO(theLogger, "ChemicalReaction constructed");
  dt_ = boost::make_shared<real>();
  interval_ = boost::make_shared<int>();

  reaction_list_ = ReactionList();
  reverse_reaction_list_ = ReactionList();

  save_pd_ = false;
  max_per_interval_ = std::numeric_limits<longint>::max();

  resetTimers();
}

ChemicalReaction::~ChemicalReaction() {
  LOG4ESPP_INFO(theLogger, "Destructor ChemicalReaction");
  disconnect();
}

/** Adds the chemical reaction to the list of reactions
 *
 * @param reaction espressopp.Reaction object.
 */
void ChemicalReaction::addReaction(boost::shared_ptr<integrator::Reaction> reaction) {
  if (!reaction->reaction_cutoff())
    throw std::runtime_error("Reaction object does not have ReactionCutoff object.");

  // Inject required objects to every reaction object.
  reaction->set_dt(dt_);
  reaction->set_interval(interval_);
  reaction->set_rng(rng_);
  reaction->setTopologyManager(tm_);

  bc::BC &bc = *getSystemRef().bc;

  reaction->reaction_cutoff()->set_bc(getSystem()->bc);
  reaction->set_system(getSystem());

  if (!reaction->reverse()) {
    // If VL cutoff is smaller than reaction, increase it.
    if (reaction->cutoff() > current_cutoff_) {
      LOG4ESPP_INFO(theLogger, "VL cutoff is extended to match with reaction cutoff");
      verlet_list_->setVerletCutoff(reaction->cutoff());
      current_cutoff_ = reaction->cutoff();
    }

    LOG4ESPP_INFO(theLogger, "Added reaction");
    reaction_list_.push_back(reaction);
  } else {
    LOG4ESPP_INFO(theLogger, "Add reverse reaction");

    // In this case, VL cutoff does not matter. Adds reaction on separate list.
    reverse_reaction_list_.push_back(reaction);
  }
}

boost::shared_ptr<integrator::Reaction> ChemicalReaction::getReaction(longint reaction_idx) {
  if (reaction_idx > reaction_list_.size())
    throw std::runtime_error("Wrong reaction idx, maximum=" + reaction_list_.size());
  return reaction_list_[reaction_idx];
}

/** Performs all steps of the reactive scheme. */
void ChemicalReaction::React() {
  if (integrator->getStep() % (*interval_) != 0)
    return;

  System &system = getSystemRef();

  LOG4ESPP_TRACE(theLogger, "Perform ChemicalReaction");

  *dt_ = integrator->getTimeStep();

  potential_pairs_.clear();
  effective_pairs_.clear();

  // loop over VL pairs
  for (PairList::Iterator it(verlet_list_->getPairs()); it.isValid(); ++it) {
    Particle &p1 = *it->first;
    Particle &p2 = *it->second;
    int reaction_idx_ = 0;

    for (ReactionList::iterator it = reaction_list_.begin(); it != reaction_list_.end(); ++it, ++reaction_idx_) {
      if (!(*it)->active())  // if raction is not active, skip it.
        continue;

      ReactedPair p;

      if ((*it)->isValidPair(p1, p2, p)) {
        longint pid1 = p.first->id();
        longint pid2 = p.second->id();
        int order = 1;
        if (pid1 > pid2) {
          order = 2;
          std::swap(pid1, pid2);
        }
        potential_pairs_.insert(
            std::make_pair(pid1, std::make_pair(pid2, ReactionDef(reaction_idx_, p.reaction_rate, p.r_sqr, order))));
      }
    }
  }  // end loop over VL pairs

  LOG4ESPP_DEBUG(theLogger, "found " << potential_pairs_.size() << " potential pairs to react");

  sendMultiMap(potential_pairs_);

  // Here, reduce number of partners to each A to 1
  // Also, keep only non-ghost A
  uniqueA(potential_pairs_);
  sendMultiMap(potential_pairs_);
  // Here, reduce number of partners to each B to 1
  // Also, keep only non-ghost B
  uniqueB(potential_pairs_, effective_pairs_);
  // Distribute effective pairs
  sendMultiMap(effective_pairs_);

  sortParticleReactionList(effective_pairs_);

  // Use effective_pairs_ to apply the reaction.
  std::set<Particle *> modified_particles;

  // First, remove pairs.
  applyDR(modified_particles);

  // Synchronize, all cpus should finish dissocition part.
  (*system.comm).barrier();

  // Now, accept new pairs.
  applyAR(modified_particles);

  // Synchronize, all cpus should finish association part.
  (*system.comm).barrier();

  // Update the ghost particles.
  updateGhost(modified_particles);

  if (save_pd_) {
    savePairDistances(pd_filename_);
  }

  LOG4ESPP_TRACE(theLogger, "Leaving react()");
}

void ChemicalReaction::printMultiMap(ReactionMap &rmap, std::string comment) {
  System &system = getSystemRef();

  for (integrator::ReactionMap::iterator it = rmap.begin(); it != rmap.end(); it++) {
    std::cout << comment << "mm on\t" << system.comm->rank() << "\t" << it->first << "\t" << it->second.first
        << "\t" << it->second.second.reaction_id << "\t"
        << it->second.second.reaction_rate << "\t" << it->second.second.reaction_r_sqr << std::endl;
  }
}

/** Performs two-way parallel communication to consolidate mm between
   neighbours. The parallel scheme is taken from
   storage::DomainDecomposition::doGhostCommunication
 */
void ChemicalReaction::sendMultiMap(integrator::ReactionMap &mm) {// NOLINT
  LOG4ESPP_TRACE(theLogger, "Entering sendMultiMap");

  System &system = getSystemRef();

  InBuffer in_buffer_0(*system.comm);
  InBuffer in_buffer_1(*system.comm);
  OutBuffer out_buffer(*system.comm);
  const storage::NodeGrid &node_grid = domdec_->getNodeGrid();

  // Prepare out buffer with the reactions that potential will happen on this node.
  out_buffer.reset();
  in_buffer_0.reset();
  in_buffer_1.reset();

  // Fill out_buffer from mm.
  int array_size = mm.size();
  int particle_id_1, particle_id_2, reaction_id;

  real reaction_rate, r_sqr;
  int order;

  out_buffer.write(array_size);

  for (integrator::ReactionMap::iterator it = mm.begin(); it != mm.end();
      it++) {
    particle_id_1 = it->first;  // particle id
    particle_id_2 = it->second.first;  // particle id
    reaction_id = it->second.second.reaction_id;  // reaction id
    reaction_rate = it->second.second.reaction_rate;  // reaction rate for this pair.
    r_sqr = it->second.second.reaction_r_sqr;  // reaction distance for this pair.
    order = it->second.second.order;
    out_buffer.write(particle_id_1);
    out_buffer.write(particle_id_2);
    out_buffer.write(reaction_id);
    out_buffer.write(reaction_rate);
    out_buffer.write(r_sqr);
    out_buffer.write(order);
  }

  LOG4ESPP_DEBUG(theLogger, "OutBuffer.size=" << out_buffer.getSize());

  /* direction loop: x, y, z.
     Here we could in principle build in a one sided ghost
     communication, simply by taking the lr loop only over one
     value. */

  int data_length, idx_a, idx_b, reaction_idx, direction_size;
  real reaction_r_sqr;
  int p_order_;

  for (int direction = 0; direction < 3; ++direction) {
    /* inverted processing order for ghost force communication,
       since the corner ghosts have to be collected via several
       nodes. We now add back the corner ghost forces first again
       to ghost forces, which only eventually go back to the real
       particle.
     */
    direction_size = node_grid.getGridSize(direction);

    if (direction_size == 1) {
      LOG4ESPP_DEBUG(theLogger, "No communication needed.");
      continue;
    }

    // lr loop: left right
    for (int left_right_dir = 0; left_right_dir < 2; ++left_right_dir) {
      // Avoids double communication for size 2 directions.
      if ((direction_size == 2) && (left_right_dir == 1))
        continue;

      // prepare send and receive buffers
      longint receiver, sender;

      receiver = node_grid.getNodeNeighborIndex(2 * direction + left_right_dir);
      sender = node_grid.getNodeNeighborIndex(2 * direction + (1 - left_right_dir));

      // exchange particles, odd-even rule. getNodePosition returns the position
      // of the current node.
      if (node_grid.getNodePosition(direction) % 2 == 0) {
        // sending the data
        out_buffer.send(receiver, kCrCommTag);

        // receiving the data
        if (left_right_dir == 0) {
          in_buffer_0.recv(sender, kCrCommTag);
        } else {
          in_buffer_1.recv(sender, kCrCommTag);
        }
      } else {
        // receiving the data
        if (left_right_dir == 0) {
          in_buffer_0.recv(sender, kCrCommTag);
        } else {
          in_buffer_1.recv(sender, kCrCommTag);
        }

        // sending the data
        out_buffer.send(receiver, kCrCommTag);
      }
    }

    LOG4ESPP_TRACE(theLogger, "Entering unpack");

    // Unpacking phase. Get the content from buffers and put in output multimap.
    for (int left_right_dir = 0; left_right_dir < 2; ++left_right_dir) {
      // Avoids double communication for size 2 directions.
      if ((direction_size == 2) && (left_right_dir == 1))
        continue;

      if (left_right_dir == 0) {
        in_buffer_0.read(data_length);
      } else {
        in_buffer_1.read(data_length);
      }

      for (longint i = 0; i < data_length; i++) {
        if (left_right_dir == 0) {
          in_buffer_0.read(idx_a);
          in_buffer_0.read(idx_b);
          in_buffer_0.read(reaction_idx);
          in_buffer_0.read(reaction_rate);
          in_buffer_0.read(reaction_r_sqr);
          in_buffer_0.read(p_order_);
        } else {
          in_buffer_1.read(idx_a);
          in_buffer_1.read(idx_b);
          in_buffer_1.read(reaction_idx);
          in_buffer_1.read(reaction_rate);
          in_buffer_1.read(reaction_r_sqr);
          in_buffer_1.read(p_order_);
        }

        mm.insert(
            std::make_pair(
                idx_a, std::make_pair(
                    idx_b,
                    ReactionDef(reaction_idx, reaction_rate, reaction_r_sqr, p_order_))));
      }
    }
    LOG4ESPP_TRACE(theLogger, "Leaving unpack");
  }

  LOG4ESPP_TRACE(theLogger, "Leaving sendMultiMap");
}

void ChemicalReaction::sortParticleReactionList(ReactionMap &mm) {
  LOG4ESPP_TRACE(theLogger, "Entering sortParticleReactionList");

  ReactionMap out;
  out.clear();
  longint idx_a, idx_b, reaction_idx;
  real reaction_rate, reaction_r_sqr;
  int p_order;

  boost::unordered_set<longint> particle_idx;  // stores particle indexes
  boost::unordered_set<longint> residue_idx;  // stores residue indexes

  for (ReactionMap::iterator it = mm.begin(); it != mm.end(); it++) {
    idx_a = it->first;  // particle id
    idx_b = it->second.first;  // particle id

    // skip particle pairs that are already in the list.
    if (particle_idx.count(idx_a) != 0 || particle_idx.count(idx_b) != 0) {
      LOG4ESPP_DEBUG(theLogger, "skip pair " << idx_a << "-" << idx_b);
      continue;
    }

    // residues
    longint rid1 = tm_->getResId(idx_a);
    longint rid2 = tm_->getResId(idx_b);

    // Skip if the residues are already in the connection list.
    if (residue_idx.find(rid1) != residue_idx.end() || residue_idx.find(rid2) != residue_idx.end()) {
      LOG4ESPP_DEBUG(theLogger, "skip pair " << idx_a << "-" << idx_b << " residues already in the reaction");
      continue;
    }

    // Keep order correct.
    if (idx_a > idx_b) {
      std::swap(idx_a, idx_b);
      if (p_order == 1)
        p_order = 2;
      else
        p_order = 1;
    }

    reaction_idx = it->second.second.reaction_id;  // reaction id
    reaction_rate = it->second.second.reaction_rate;   // reaction rate for this pair.
    reaction_r_sqr = it->second.second.reaction_r_sqr;  // reaction distance for this pair.
    p_order = it->second.second.order;

    out.insert(
        std::make_pair(
            idx_a, std::make_pair(idx_b, ReactionDef(reaction_idx, reaction_rate, reaction_r_sqr, p_order))));
    // insert used particle idx to the set.
    particle_idx.insert(idx_a);
    particle_idx.insert(idx_b);
    residue_idx.insert(rid1);
    residue_idx.insert(rid2);
  }

  // Make pairs unique among CPUs, use master process to handle it.
  std::vector<ReactionMap> global_maps;
  if (getSystem()->comm->rank() == 0) {
    particle_idx.clear();
    residue_idx.clear();

    std::map<longint, std::set<longint> > residue_map;
    std::map<longint, std::set<longint> > molecule_map;

    // Collect maps from all cpus.
    mpi::gather(*(getSystem()->comm), out, global_maps, 0);

    mm.clear();  // clear mm, it's already in global_maps
    // iterate over CPUs maps and check the particle lists. First In First Served idea;

    longint reaction_counter = 0;

    for (std::vector<ReactionMap>::iterator it_rms = global_maps.begin(); it_rms != global_maps.end(); it_rms++) {
      for (ReactionMap::iterator it = it_rms->begin(); it != it_rms->end() && reaction_counter < max_per_interval_; it++) {
        longint idx_a = it->first;
        longint idx_b = it->second.first;
        shared_ptr<integrator::Reaction> reaction = reaction_list_[it->second.second.reaction_id];
        longint rid1 = tm_->getResId(idx_a);
        longint rid2 = tm_->getResId(idx_b);
        longint mid1 = tm_->getMoleculeId(idx_a);
        longint mid2 = tm_->getMoleculeId(idx_b);

        bool valid = true;
        if (!reaction->intraresidual()) {
          valid = !(residue_map.count(rid1) == 1 && residue_map.at(rid1).count(rid2) == 1);
        }
        if (valid && !reaction->intramolecular()) {
          valid &= !(molecule_map.count(mid1) == 1 && molecule_map.at(mid1).count(mid2) == 1);
          valid &= !tm_->isSameMolecule(idx_a, idx_b);
        }

        if (valid) {
          valid &= (particle_idx.find(idx_a) == particle_idx.end() && particle_idx.find(idx_b) == particle_idx.end());
          valid &= (residue_idx.find(rid1) == residue_idx.end() && residue_idx.find(rid2) == residue_idx.end());
        }

        if (valid) {
          // particle in use
          particle_idx.insert(idx_a);
          particle_idx.insert(idx_b);
          // residues in use;
          residue_idx.insert(rid1);
          residue_idx.insert(rid2);
          // update local residue map
          residue_map[rid1].insert(rid2);
          residue_map[rid2].insert(rid1);
          // update local molecule map
          molecule_map[mid1].insert(mid2);
          molecule_map[mid2].insert(mid1);
          // insert part in output list.
          mm.insert(std::make_pair(idx_a, std::make_pair(idx_b, it->second.second)));
          reaction_counter++;
        }
      }
    }
    mpi::broadcast(*(getSystem()->comm), mm, 0);
  } else {
    // send local reaction map to root cpu.
    mpi::gather(*(getSystem()->comm), out, global_maps, 0);
    mm.clear();
    mpi::broadcast(*(getSystem()->comm), mm, 0);
  }

  LOG4ESPP_TRACE(theLogger, "Leaving sortParticleReactionList");
}

/** Performs two-way parallel communication to update the ghost particles.
 * The parallel scheme is taken from
 * storage::DomainDecomposition::doGhostCommunication
 */
void ChemicalReaction::updateGhost(const std::set<Particle *> &modified_particles) {// NOLINT
  LOG4ESPP_TRACE(theLogger, "Entering updateGhost");

  System &system = getSystemRef();

  InBuffer in_buffer_0(*getSystem()->comm);
  InBuffer in_buffer_1(*getSystem()->comm);
  OutBuffer out_buffer(*getSystem()->comm);
  const storage::NodeGrid &node_grid = domdec_->getNodeGrid();

  // Prepare out buffer with the reactions that potential will happen on this node.
  out_buffer.reset();
  in_buffer_0.reset();
  in_buffer_1.reset();

  // Fill out_buffer from the particles properties.
  longint data_length = modified_particles.size();
  longint p_id, p_type, p_res_id, p_state;
  real p_mass, p_q, p_lambda;

  out_buffer.write(data_length);

  for (std::set<Particle *>::const_iterator it = modified_particles.begin();
      it != modified_particles.end();
      ++it) {
    p_id = (*it)->id();
    p_type = (*it)->type();
    p_mass = (*it)->mass();
    p_q = (*it)->q();
    p_res_id = (*it)->res_id();
    p_lambda = (*it)->lambda();
    p_state = (*it)->state();
    out_buffer.write(p_id);
    out_buffer.write(p_type);
    out_buffer.write(p_mass);
    out_buffer.write(p_q);
    out_buffer.write(p_res_id);
    out_buffer.write(p_lambda);
    out_buffer.write(p_state);
  }

  // Temporary data.
  Particle *particle = NULL;
  int direction_size = 0;

  /* direction loop: x, y, z.
     Here we could in principle build in a one sided ghost
     communication, simply by taking the lr loop only over one
     value. */
  for (int direction = 0; direction < 3; ++direction) {
    /* inverted processing order for ghost force communication,
       since the corner ghosts have to be collected via several
       nodes. We now add back the corner ghost forces first again
       to ghost forces, which only eventually go back to the real
       particle.
     */
    direction_size = node_grid.getGridSize(direction);

    if (direction_size == 1) {
      LOG4ESPP_DEBUG(theLogger, "No communication needed.");
      continue;
    }

    // lr loop: left right
    for (int left_right_dir = 0; left_right_dir < 2; ++left_right_dir) {
      // Avoids double communication for size 2 directions.
      if ((direction_size == 2) && (left_right_dir == 1))
        continue;

      // prepare send and receive buffers
      longint receiver, sender;

      receiver = node_grid.getNodeNeighborIndex(2 * direction + left_right_dir);
      sender = node_grid.getNodeNeighborIndex(2 * direction + (1 - left_right_dir));

      // exchange particles, odd-even rule. getNodePosition returns the position
      // of the current node.
      if (node_grid.getNodePosition(direction) % 2 == 0) {
        // sending the data
        out_buffer.send(receiver, kCrCommTag);

        // receiving the data
        if (left_right_dir == 0) {
          in_buffer_0.recv(sender, kCrCommTag);
        } else {
          in_buffer_1.recv(sender, kCrCommTag);
        }
      } else {
        // receiving the data
        if (left_right_dir == 0) {
          in_buffer_0.recv(sender, kCrCommTag);
        } else {
          in_buffer_1.recv(sender, kCrCommTag);
        }

        // sending the data
        out_buffer.send(receiver, kCrCommTag);
      }
    }

    LOG4ESPP_DEBUG(theLogger, "Entering unpack");

    // Unpacking phase. Get the parameters and set the data to particles.
    for (int left_right_dir = 0; left_right_dir < 2; ++left_right_dir) {
      // Avoids double communication for size 2 directions.
      if ((direction_size == 2) && (left_right_dir == 1))
        continue;

      if (left_right_dir == 0) {
        in_buffer_0.read(data_length);
      } else {
        in_buffer_1.read(data_length);
      }

      for (longint i = 0; i < data_length; i++) {
        if (left_right_dir == 0) {
          in_buffer_0.read(p_id);
          in_buffer_0.read(p_type);
          in_buffer_0.read(p_mass);
          in_buffer_0.read(p_q);
          in_buffer_0.read(p_res_id);
          in_buffer_0.read(p_lambda);
          in_buffer_0.read(p_state);
        } else {
          in_buffer_1.read(p_id);
          in_buffer_1.read(p_type);
          in_buffer_1.read(p_mass);
          in_buffer_1.read(p_q);
          in_buffer_1.read(p_res_id);
          in_buffer_1.read(p_lambda);
          in_buffer_1.read(p_state);
        }

        // Update the ghost particle data on neighbour CPUs.
        particle = system.storage->lookupLocalParticle(p_id);

        if (particle != NULL && particle->ghost()) {
          LOG4ESPP_DEBUG(theLogger, "Update particle data");
          particle->setType(p_type);
          particle->setMass(p_mass);
          particle->setQ(p_q);
          particle->setResId(p_res_id);
          particle->setLambda(p_lambda);
          particle->setState(p_state);
        }
      }
    }

    LOG4ESPP_TRACE(theLogger, "Leaving unpack");
  }

  LOG4ESPP_TRACE(theLogger, "Leaving updateGhost");
}

/** Given a multimap mm with several pairs (id1,id2), keep only one pair for
   each id1 and return it in place. In addition, only pairs for which
   id1 is local are kept.
 */
void ChemicalReaction::uniqueA(integrator::ReactionMap &potential_candidates) {// NOLINT
  LOG4ESPP_TRACE(theLogger, "uniqueA");

  System &system = getSystemRef();
  integrator::ReactionMap unique_list_of_candidates;
  boost::unordered_set<longint> a_indexes;

  Particle *p = NULL;

  unique_list_of_candidates.clear();

  // Gets the list of indexes of particle a. Gets only real particles, skip ghost.
  for (integrator::ReactionMap::iterator it = potential_candidates.begin();
      it != potential_candidates.end(); ++it) {
    p = system.storage->lookupRealParticle(it->first);

    if (p == NULL)
      continue;

    a_indexes.insert(it->first);
  }

  // For each active idx1, pick a partner
  if (a_indexes.size() > 0) {
    int idx_a;
    real rc;
    real max_rc;

    // reaction_coordinate => idx_b, ReactionDef(reaction_id, reaction_rate) or r_sqr distance
    typedef std::multimap<real, std::pair<longint, ReactionDef > > LocalDistanceIdx;
    LocalDistanceIdx rc_idx_b;
    LocalDistanceIdx::iterator idx_b_reaction_id;

    typedef std::vector<std::pair<longint, ReactionDef> > LocalCandidateIdx;
    LocalCandidateIdx candidates_idx_b;

    // Iterators for the equal_range.
    std::pair<integrator::ReactionMap::iterator, integrator::ReactionMap::iterator> candidates_b;

    // Iterate over the ids of particle A, looking for the particle B
    for (boost::unordered_set<longint>::iterator it = a_indexes.begin(); it != a_indexes.end(); ++it) {
      idx_a = *it;

      // Group the candidates by the reaction coordinate, rate or distance.
      if (is_nearest_) {  // silly
        max_rc = 10e18;
        rc_idx_b.clear();
      } else {
        candidates_idx_b.clear();
      }

      // Select all possible candidates of particle A
      candidates_b = potential_candidates.equal_range(idx_a);
      // Iterate over potential candidates
      for (integrator::ReactionMap::iterator jt = candidates_b.first; jt != candidates_b.second; ++jt) {
        real reaction_rate = jt->second.second.reaction_rate;
        real reaction_r_sqr = jt->second.second.reaction_r_sqr;

        if (is_nearest_) {  // select nearest neighbour.
          if (reaction_r_sqr < max_rc)
            max_rc = reaction_r_sqr;
          // distance => (idx_b, reaction_id)
          rc_idx_b.insert(std::make_pair(reaction_r_sqr, std::make_pair(jt->second.first, jt->second.second)));
        } else {  // select randomly
          candidates_idx_b.push_back(std::make_pair(jt->second.first, jt->second.second));
        }
      }  // end preparing candidates of A.

      // Decide, select randomly candidate or select closes neighbour
      if (is_nearest_) {
        int bucket_size = rc_idx_b.count(max_rc);

        int pick_offset = 0;
        if (bucket_size > 1)
          // Pick up random number in given range.
          pick_offset = (*rng_)(bucket_size-1);

        LocalDistanceIdx::iterator idx_b_reaction_id = rc_idx_b.lower_bound(max_rc);

        std::advance(idx_b_reaction_id, pick_offset);

        unique_list_of_candidates.insert(
            std::make_pair(idx_a,
                           std::make_pair(
                               idx_b_reaction_id->second.first,
                               idx_b_reaction_id->second.second)));
      } else if (candidates_idx_b.size() > 0) {
        longint candidate_length = candidates_idx_b.size();
        longint candidate_index = (*rng_)(candidate_length);

        std::pair<longint, ReactionDef> selected_b = candidates_idx_b[candidate_index];
        unique_list_of_candidates.insert(std::make_pair(idx_a, std::make_pair(selected_b.first, selected_b.second)));
      }
    }
  }

  //  @todo(jakub): I'm not sure if this is an efficient approach.
  potential_candidates.clear();
  potential_candidates = unique_list_of_candidates;
}

/** Given a multimap mm with several pairs (id1,id2), keep only one pair for
   each id2 and return it in place. In addition, only pairs for which
   id2 is local are kept.
 */
void ChemicalReaction::uniqueB(integrator::ReactionMap &potential_candidates,// NOLINT
                               integrator::ReactionMap &effective_candidates) {// NOLINT
  LOG4ESPP_TRACE(theLogger, "uniqueB");

  typedef boost::unordered_set<longint> Indexes;
  typedef std::multimap<real, std::pair<longint, ReactionDef> > DistParticleIdx;
  typedef std::vector<std::pair<longint, ReactionDef> > LocalCandidateIdx;

  System &system = getSystemRef();
  Indexes b_indexes;
  integrator::ReactionMap reverse_candidates;

  Particle *p = NULL;

  effective_candidates.clear();

  // Collect the b particle pairs. REQOPT
  for (integrator::ReactionMap::iterator it = potential_candidates.begin();
      it != potential_candidates.end(); ++it) {
    p = system.storage->lookupRealParticle(it->second.first);

    if (p == NULL)  // only real particles
      continue;

    b_indexes.insert(it->second.first);
    reverse_candidates.insert(
        std::make_pair(it->second.first,  // idB
                       std::make_pair(
                           it->first,     // idA
                           it->second.second)));  // ReactionDef
  }

  if (b_indexes.size() > 0) {
    int idx_b;
    real max_rc;
    real rc;

    // rate => idx_a, reaction_id, reaction_rate
    DistParticleIdx rc_idx_a;
    DistParticleIdx::iterator idx_a_reaction_id;
    std::pair<integrator::ReactionMap::iterator, integrator::ReactionMap::iterator> candidates_a;

    LocalCandidateIdx candidates_idx_a;
    // Iterate over b particles
    for (Indexes::iterator it = b_indexes.begin(); it != b_indexes.end(); ++it) {
      idx_b = *it;

      // Group the candidates by the reaction rate.
      if (is_nearest_) {  // silly
        max_rc = 10e18;
        rc_idx_a.clear();
      } else {
        candidates_idx_a.clear();
      }

      // Iterates over potential candidates A.
      candidates_a = reverse_candidates.equal_range(idx_b);
      for (integrator::ReactionMap::iterator jt = candidates_a.first; jt != candidates_a.second; ++jt) {
        real reaction_rate = jt->second.second.reaction_rate;
        real reaction_r_sqr = jt->second.second.reaction_r_sqr;

        if (is_nearest_) {
          if (reaction_r_sqr < max_rc)
            max_rc = reaction_r_sqr;
          rc_idx_a.insert(
              std::make_pair(reaction_r_sqr, std::make_pair(jt->second.first, jt->second.second)));
        } else {
          candidates_idx_a.push_back(std::make_pair(jt->second.first, jt->second.second));
        }
      }

      // Decide, select randomly candidate or select closest neighbour
      if (is_nearest_) {
        int bucket_size = rc_idx_a.count(max_rc);
        int pick_offset = 0;
        if (bucket_size > 1)
          pick_offset = (*rng_)(bucket_size - 1);

        idx_a_reaction_id = rc_idx_a.lower_bound(max_rc);

        std::advance(idx_a_reaction_id, pick_offset);

        effective_candidates.insert(
            std::make_pair(
                idx_a_reaction_id->second.first,
                std::make_pair(idx_b,
                               idx_a_reaction_id->second.second)));
      } else {  // pick randomly
        longint candidate_length = candidates_idx_a.size();
        longint candidate_index = (*rng_)(candidate_length);

        std::pair<longint, ReactionDef> selected_a = candidates_idx_a[candidate_index];
        effective_candidates.insert(
            std::make_pair(selected_a.first, std::make_pair(idx_b, selected_a.second)));
      }
    }
  }
}

/** ApplyDR to remove bonds and change the state of the particles
 * accordingly
 */
void ChemicalReaction::applyDR(std::set<Particle *> &modified_particles) {
  LOG4ESPP_TRACE(theLogger, "Entering applyDR");

  // Iterate over reverse reaction. For every reaction, iterate overy particles pairs and
  // decide to remove or keep bond.
  std::set<Particle *> tmp;

  longint reaction_idx = 0;
  for (ReactionList::iterator it = reverse_reaction_list_.begin(); it != reverse_reaction_list_.end(); ++it) {
    integrator::Reaction &r = **it;

    if (!r.active())
      continue;

    bool updated_fpl = false;

    for (FixedPairList::PairList::Iterator itp(*((*it)->fixed_pair_list_)); itp.isValid(); ++itp) {
      Particle &p1 = *itp->first;
      Particle &p2 = *itp->second;
      ReactedPair p;

      if ((*it)->isValidPair(p1, p2, p)) {
        // Ok we will remove this pair.
        if (!p1.ghost() && !p1.ghost())
          (*it)->fixed_pair_list_->remove(p1.id(), p2.id());
        else if (p2.ghost())
          (*it)->fixed_pair_list_->remove(p1.id(), p2.id());
        else
          (*it)->fixed_pair_list_->remove(p2.id(), p1.id());

        // Run postprocess.
        p.first->setState(p.first->getState() + r.delta_1());
        tmp = r.postProcess_T1(*p.first, *p.second);
        modified_particles.insert(tmp.begin(), tmp.end());

        p.second->setState(p.second->getState() + r.delta_2());
        tmp = r.postProcess_T2(*p.second, *p.first);
        modified_particles.insert(tmp.begin(), tmp.end());
        updated_fpl = true;
      }
    }
    reaction_idx++;

    // Trigger update of FixedPairList.
    if (updated_fpl)
      (*it)->fixed_pair_list_->updateParticlesStorage();
  }
  LOG4ESPP_TRACE(theLogger, "Leaving applyDR");
}

/** Use the (A,B) list "partners" to add bonds and change the state of the
   particles accordingly.
 */
void ChemicalReaction::applyAR(std::set<Particle *> &modified_particles) {
  System &system = getSystemRef();
  std::set<Particle *> tmp;

  LOG4ESPP_TRACE(theLogger, "Entering applyAR");

  std::vector<longint> tmp_reaction_counters;
  tmp_reaction_counters.resize(reaction_list_.size());
  std::vector<longint> tmp_num_intra_inter(2);
  tmp_num_intra_inter[0] = 0;
  tmp_num_intra_inter[1] = 0;

  for (integrator::ReactionMap::iterator it = effective_pairs_.begin(); it != effective_pairs_.end(); it++) {
    boost::shared_ptr<integrator::Reaction> reaction = reaction_list_.at(it->second.second.reaction_id);

    Particle *p1;
    Particle *p2;
    // Change the state of A and B.
    if (it->second.second.order == 1) {
      p1 = system.storage->lookupLocalParticle(it->first);
      p2 = system.storage->lookupLocalParticle(it->second.first);
    } else if (it->second.second.order == 2) {
      p1 = system.storage->lookupLocalParticle(it->second.first);
      p2 = system.storage->lookupLocalParticle(it->first);
    } else {
      LOG4ESPP_ERROR(theLogger, "wrong order parameter " << it->second.second.order);
    }

#ifdef LOG4ESPP_DEBUG_ENABLED
    if (p1 && p2) {
      LOG4ESPP_DEBUG(
          theLogger,
          "Checking pair: " << p1->id() << "(st=" << p1->state() << ")-"
              << p2->id() << "(st=" << p2->state() << ") A.type="
              << p1->type() << " B.type=" << p2->type());
    }
#endif

    bool valid_state = true;

    if (p1 && p2) {
      valid_state = (reaction->type_1() == p1->type() && reaction->isValidState_T1(*p1));
      valid_state &= (reaction->type_2() == p2->type() && reaction->isValidState_T2(*p2));
      // Whole pair has to be valid before the state can be changed.
      if (valid_state) {
        longint old_state = p1->getState();
        p1->setState(p1->getState() + reaction->delta_1());
        tmp = reaction->postProcess_T1(*p1, *p2);
        modified_particles.insert(tmp.begin(), tmp.end());
        LOG4ESPP_DEBUG(
            theLogger,
            "postProcess_T1: " << p1->id() << " st=" << old_state << "->" << p1->getState());

        old_state = p2->getState();
        p2->setState(p2->getState() + reaction->delta_2());
        tmp = reaction->postProcess_T2(*p2, *p1);
        modified_particles.insert(tmp.begin(), tmp.end());
        LOG4ESPP_DEBUG(
            theLogger,
            "postProcess_T2: " << p2->id() << " st=" << old_state << "->" << p2->getState());

        modified_particles.insert(p1);
        modified_particles.insert(p2);
        /** Make sense only if both particles exists here, otherwise waste of CPU time. */
        if (!reaction->virtual_reaction() && !(p1->ghost() && p2->ghost())) {  // if both ghost then skip it
          bool retval = reaction->fixed_pair_list_->iadd(it->first, it->second.first);
          if (retval) {
            LOG4ESPP_DEBUG(theLogger, "added pair " << it->first << "-" << it->second.first);
            tmp_reaction_counters[it->second.second.reaction_id]++;
            if (save_pd_)
              pair_distances_.push_back(it->second.second.reaction_r_sqr);
            // Count intra and intermolecular reactions.
            longint mol_id1 = tm_->getMoleculeId(it->first);
            longint mol_id2 = tm_->getMoleculeId(it->second.first);
            if (mol_id1 == mol_id2)
              tmp_num_intra_inter[0]++;
            else
              tmp_num_intra_inter[1]++;
          }
        }
      }
    }
  }

  // Saves some statistics.
  longint current_step = integrator->getStep();
  time_reaction_counter_.insert(std::make_pair(current_step, tmp_reaction_counters));
  intra_inter_reaction_counter_.insert(std::make_pair(current_step, tmp_num_intra_inter));

  LOG4ESPP_DEBUG(theLogger, "Leaving applyAR, modified_particles: " << modified_particles.size());
}

void ChemicalReaction::disconnect() {
  react_.disconnect();
}

void ChemicalReaction::connect() {
  react_ = integrator->aftIntV.connect(extensionOrder, boost::bind(&ChemicalReaction::React, this));
}

python::list ChemicalReaction::getTimers() {
  python::list ret;
  ret.append(python::make_tuple("timeComm", timeComm));
  ret.append(python::make_tuple("timeUpdateGhost", timeUpdateGhost));
  ret.append(python::make_tuple("timeApplyAR", timeApplyAR));
  ret.append(python::make_tuple("timeApplyDR", timeApplyDR));
  ret.append(python::make_tuple("timeLoopPair", timeLoopPair));
  ret.append(python::make_tuple("timeAll", timeComm + timeUpdateGhost + timeApplyAR + timeApplyDR + timeLoopPair));

  return ret;
}

void ChemicalReaction::savePairDistances(std::string filename) {
  std::vector<std::vector<real> > allDistances;
  System &system = getSystemRef();
  // Collect data from all CPUs.
  if (system.comm->rank() == 0) {
    mpi::gather(*(system.comm), pair_distances_, allDistances, 0);

    // Write data to file.
    std::ofstream output_file;
    output_file.open(filename.c_str(), std::ofstream::out|std::ofstream::app);

    for (std::vector<std::vector<real> >::iterator it = allDistances.begin(); it != allDistances.end(); ++it) {
      for (std::vector<real>::iterator itv = it->begin(); itv != it->end(); ++itv) {
        output_file << *itv << "\n";
      }
    }

    output_file.close();
  } else {
    mpi::gather(*(system.comm), pair_distances_, allDistances, 0);
  }
  pair_distances_.clear();
}
python::list ChemicalReaction::getPairDistances() {
  python::list ret_list;
  for (std::vector<real>::iterator it = pair_distances_.begin(); it != pair_distances_.end(); ++it) {
    ret_list.append(*it);
  }
  return ret_list;
}

python::list ChemicalReaction::getReactionCounters() {
  python::list ret_list;
  typedef std::map<longint, std::vector<longint> > ARC;
  std::vector<ARC> all_data;

  System &system = getSystemRef();
  // Collect data from all CPUs.
  if (system.comm->rank() == 0) {
    mpi::gather(*system.comm, time_reaction_counter_, all_data, 0);
    ARC time_reaction_counters;

    for (std::vector<ARC>::iterator it = all_data.begin(); it != all_data.end(); it++) {
      for (ARC::iterator itt = it->begin(); itt != it->end(); ++itt) {
        if (time_reaction_counters.find(itt->first) == time_reaction_counters.end()) {
          time_reaction_counters[itt->first] = itt->second;
        } else {
          for (longint i = 0; i < itt->second.size(); i++) {
            time_reaction_counters[itt->first][i] += itt->second[i];
          }
        }
      }
    }
    // output data.
    for (ARC::iterator it = time_reaction_counters.begin(); it != time_reaction_counters.end(); ++it) {
      python::list tmp_list;
      tmp_list.append(it->first);
      for (std::vector<longint>::iterator itt = it->second.begin(); itt != it->second.end(); ++itt) {
        tmp_list.append(*itt);
      }
      ret_list.append(tmp_list);
    }
  } else {
    mpi::gather(*system.comm, time_reaction_counter_, all_data, 0);
  }
  return ret_list;
}

python::list ChemicalReaction::getReactionNumIntraInterCounters() {
  python::list ret_list;
  typedef std::map<longint, std::vector<longint> > ARC;
  std::vector<ARC> all_data;

  System &system = getSystemRef();
  if (system.comm->rank() == 0) {
    mpi::gather(*system.comm, intra_inter_reaction_counter_, all_data, 0);
    ARC time_reaction_num_intra_inter;

    // Collect data from other cpus.
    for (std::vector<ARC>::iterator it = all_data.begin(); it != all_data.end(); it++) {
      for (ARC::iterator itt = it->begin(); itt != it->end(); ++itt) {
        // Check if timestep exists in result map, if not then set it.
        if (time_reaction_num_intra_inter.find(itt->first) == time_reaction_num_intra_inter.end()) {
          time_reaction_num_intra_inter.insert(std::make_pair(itt->first, std::vector<longint>(2)));
          time_reaction_num_intra_inter[itt->first][0] = itt->second[0];
          time_reaction_num_intra_inter[itt->first][1] = itt->second[1];
        } else {
          time_reaction_num_intra_inter[itt->first][0] += itt->second[0];
          time_reaction_num_intra_inter[itt->first][1] += itt->second[1];
        }
      }
    }

    // Output data.
    for (ARC::iterator it = time_reaction_num_intra_inter.begin(); it != time_reaction_num_intra_inter.end(); ++it) {
      python::list tmp_list;
      tmp_list.append(it->first);
      tmp_list.append(it->second[0]);  // intra mol1==mol2
      tmp_list.append(it->second[1]);  // inter mol1 != mol2
      ret_list.append(tmp_list);
    }
  } else {
    mpi::gather(*system.comm, intra_inter_reaction_counter_, all_data, 0);
  }

  return ret_list;
}

void ChemicalReaction::registerPython() {
  using namespace espressopp::python;// NOLINT
  class_<ChemicalReaction, shared_ptr<ChemicalReaction>, bases<Extension> >(
      "integrator_ChemicalReaction",
      init<shared_ptr<System>, shared_ptr<VerletList>,
      shared_ptr<storage::DomainDecomposition>, shared_ptr<TopologyManager> >())
    .def("connect", &ChemicalReaction::connect)
    .def("disconnect", &ChemicalReaction::disconnect)
    .def("add_reaction", &ChemicalReaction::addReaction)
    .def("get_reaction", &ChemicalReaction::getReaction)
    .def("get_timers", &ChemicalReaction::getTimers)
    .def("save_pair_distances", &ChemicalReaction::savePairDistances)
    .def("get_pair_distances", &ChemicalReaction::getPairDistances)
    .def("clear_pair_distances", &ChemicalReaction::clearPairDistances)
    .def("get_reaction_counters", &ChemicalReaction::getReactionCounters)
    .def("get_reaction_num_intra_inter_counters", &ChemicalReaction::getReactionNumIntraInterCounters)
    .add_property("pair_distances_filename", &ChemicalReaction::pd_filename_, &ChemicalReaction::set_pd_filename)
    .add_property("interval", &ChemicalReaction::interval, &ChemicalReaction::set_interval)
    .add_property("nearest_mode", &ChemicalReaction::is_nearest, &ChemicalReaction::set_is_nearest)
    .add_property("max_per_interval", make_getter(&ChemicalReaction::max_per_interval_), make_setter(&ChemicalReaction::max_per_interval_));
}

}  // namespace integrator
}  // namespace espressopp
