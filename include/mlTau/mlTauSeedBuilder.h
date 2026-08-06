#pragma once

#include <string>

#include "Gaudi/Algorithm.h"
#include "Gaudi/Property.h"
#include "GaudiKernel/EventContext.h"
#include "k4FWCore/DataHandle.h"

#include "edm4hep/ReconstructedParticleCollection.h"

namespace mlTau {

class mlTauSeedBuilder final : public Gaudi::Algorithm {
public:
  mlTauSeedBuilder(const std::string& name, ISvcLocator* svcLoc);

  StatusCode execute(const EventContext& ctx) const override;

private:
  Gaudi::Property<float> m_radius{
      this, "Radius", 0.4F, "FastJet ee_genkt radius parameter"};
  Gaudi::Property<float> m_genKtPower{
      this, "GenKtPower", -1.0F, "FastJet ee_genkt p parameter"};
  Gaudi::Property<float> m_minPt{
      this, "MinPt", 0.0F, "Minimum seed jet pt in GeV"};

  mutable k4FWCore::DataHandle<edm4hep::ReconstructedParticleCollection> m_inputParticles{
      "PandoraPFOs", Gaudi::DataHandle::Reader, this};
  mutable k4FWCore::DataHandle<edm4hep::ReconstructedParticleCollection> m_outputSeeds{
      "mlTauSeedJets", Gaudi::DataHandle::Writer, this};
};

} // namespace mlTau
