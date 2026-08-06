#include "mlTau/mlTauSeedBuilder.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "edm4hep/MutableReconstructedParticle.h"
#include "edm4hep/Vector3f.h"
#include "fastjet/ClusterSequence.hh"
#include "fastjet/JetDefinition.hh"
#include "fastjet/PseudoJet.hh"

DECLARE_COMPONENT(mlTau::mlTauSeedBuilder)

namespace {

float massFromP4(float px, float py, float pz, float energy) {
  const auto mass2 = energy * energy - px * px - py * py - pz * pz;
  return std::sqrt(std::max(mass2, 0.0F));
}

float energyFromMomentumAndMass(float px, float py, float pz, float mass) {
  return std::sqrt(std::max(px * px + py * py + pz * pz + mass * mass, 0.0F));
}

} // namespace

namespace mlTau {

mlTauSeedBuilder::mlTauSeedBuilder(const std::string& name, ISvcLocator* svcLoc)
    : Gaudi::Algorithm(name, svcLoc) {
  declareProperty("InputParticles", m_inputParticles,
                  "Input reconstructed particles/PFOs to cluster");
  declareProperty("OutputSeeds", m_outputSeeds,
                  "Output seed jets with linked constituents");
}

StatusCode mlTauSeedBuilder::execute(const EventContext& /*ctx*/) const {
  const auto* particles = m_inputParticles.get();
  auto* seeds = m_outputSeeds.createAndPut();

  if (particles == nullptr || particles->empty()) {
    if (msgLevel(MSG::DEBUG)) {
      debug() << "No input particles to cluster" << endmsg;
    }
    return StatusCode::SUCCESS;
  }

  std::vector<fastjet::PseudoJet> pseudoJets;
  pseudoJets.reserve(particles->size());
  for (std::size_t index = 0; index < particles->size(); ++index) {
    const auto particle = (*particles)[index];
    const auto momentum = particle.getMomentum();
    const auto energy =
        energyFromMomentumAndMass(momentum.x, momentum.y, momentum.z,
                                  static_cast<float>(particle.getMass()));
    fastjet::PseudoJet pseudoJet(momentum.x, momentum.y, momentum.z,
                                 energy);
    pseudoJet.set_user_index(static_cast<int>(index));
    pseudoJets.push_back(pseudoJet);
  }

  const fastjet::JetDefinition jetDefinition(
      fastjet::ee_genkt_algorithm, m_radius.value(), m_genKtPower.value());
  fastjet::ClusterSequence clusterSequence(pseudoJets, jetDefinition);
  auto clusteredJets = clusterSequence.inclusive_jets(m_minPt.value());

  for (const auto& clusteredJet : clusteredJets) {
    auto seed = seeds->create();
    seed.setMomentum({static_cast<float>(clusteredJet.px()),
                      static_cast<float>(clusteredJet.py()),
                      static_cast<float>(clusteredJet.pz())});
    seed.setEnergy(static_cast<float>(clusteredJet.E()));
    seed.setMass(massFromP4(static_cast<float>(clusteredJet.px()),
                            static_cast<float>(clusteredJet.py()),
                            static_cast<float>(clusteredJet.pz()),
                            static_cast<float>(clusteredJet.E())));

    auto constituents = clusteredJet.constituents();
    std::sort(constituents.begin(), constituents.end(),
              [](const fastjet::PseudoJet& lhs, const fastjet::PseudoJet& rhs) {
                return lhs.user_index() < rhs.user_index();
              });

    float charge = 0.0F;
    for (const auto& constituent : constituents) {
      const auto index = constituent.user_index();
      if (index < 0 || static_cast<std::size_t>(index) >= particles->size()) {
        continue;
      }
      const auto particle = (*particles)[static_cast<std::size_t>(index)];
      charge += static_cast<float>(particle.getCharge());
      seed.addToParticles(particle);
    }
    seed.setCharge(charge);
  }

  if (msgLevel(MSG::DEBUG)) {
    debug() << "Clustered " << particles->size() << " particles into "
            << seeds->size() << " mlTau seed jets" << endmsg;
  }
  return StatusCode::SUCCESS;
}

} // namespace mlTau
