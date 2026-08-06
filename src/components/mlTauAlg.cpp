#include "mlTau/mlTauAlg.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <stdexcept>

#include "GaudiKernel/MsgStream.h"
#include "GaudiKernel/StatusCode.h"
#include "GaudiKernel/SystemOfUnits.h"
#include "edm4hep/MutableParticleID.h"
#include "edm4hep/MutableReconstructedParticle.h"
#include "edm4hep/TrackState.h"
#include "edm4hep/Vector3f.h"
#include "nlohmann/json.hpp"

DECLARE_COMPONENT(mlTau::mlTauAlg)

namespace {

float safeLog(float value) {
  constexpr float eps = 1.0e-6F;
  return std::log(std::max(value, eps));
}

float deltaPhi(float phi1, float phi2) {
  return std::atan2(std::sin(phi1 - phi2), std::cos(phi1 - phi2));
}

float ptFromMomentum(const edm4hep::Vector3f& p) {
  return std::hypot(p.x, p.y);
}

float phiFromMomentum(const edm4hep::Vector3f& p) {
  return std::atan2(p.y, p.x);
}

float etaFromMomentum(const edm4hep::Vector3f& p) {
  const auto pt = ptFromMomentum(p);
  return std::asinh(p.z / std::max(pt, 1.0e-12F));
}

float massFromP4(float px, float py, float pz, float energy) {
  const auto mass2 = energy * energy - px * px - py * py - pz * pz;
  return std::sqrt(std::max(mass2, 0.0F));
}

float energyFromMomentumAndMass(float px, float py, float pz, float mass) {
  return std::sqrt(std::max(px * px + py * py + pz * pz + mass * mass, 0.0F));
}

float safeRatio(float numerator, float denominator) {
  return std::abs(denominator) > 1.0e-12F ? numerator / denominator : 0.0F;
}

bool isHadronPdg(int absPdg) {
  return absPdg == 130 || absPdg == 211 || absPdg == 310 || absPdg == 311 ||
         absPdg == 321 || absPdg == 2112 || absPdg == 2212 || absPdg > 1000;
}

} // namespace

namespace mlTau {

mlTauAlg::mlTauAlg(const std::string& name, ISvcLocator* svcLoc)
    : Gaudi::Algorithm(name, svcLoc) {
  declareProperty("InputJets", m_inputJets,
                  "Input seed jets as edm4hep::ReconstructedParticleCollection");
  declareProperty("InputTracks", m_inputTracks,
                  "Input track collection used for lifetime variables");
  declareProperty("InputPrimaryVertices", m_inputPrimaryVertices,
                  "Input primary vertex collection used for lifetime variables");
  declareProperty("OutputTaus", m_outputTaus,
                  "Output reconstructed tau candidates");
  declareProperty("OutputTauID", m_outputIds,
                  "Output ParticleID collection with mlTau scores");
}

StatusCode mlTauAlg::initialize() {
  if (!Gaudi::Algorithm::initialize().isSuccess()) {
    return StatusCode::FAILURE;
  }
  if (!loadMetadata().isSuccess()) {
    return StatusCode::FAILURE;
  }
  if (!initializeOnnx().isSuccess()) {
    return StatusCode::FAILURE;
  }
  info() << "Initialized mlTau with model=" << m_modelPath
         << ", metadata=" << m_metadataPath
         << ", input scaling=" << (m_metadata.inputScalingEnabled ? "enabled" : "disabled")
         << endmsg;
  return StatusCode::SUCCESS;
}

StatusCode mlTauAlg::loadMetadata() {
  std::ifstream input(m_metadataPath.value());
  if (!input) {
    error() << "Could not open metadata JSON: " << m_metadataPath << endmsg;
    return StatusCode::FAILURE;
  }

  nlohmann::json metadata;
  input >> metadata;

  const auto scaling = metadata.value("input_scaling", nlohmann::json::object());
  m_metadata.inputScalingEnabled = scaling.value("enabled", false);
  if (m_metadata.inputScalingEnabled) {
    m_metadata.scalingFeatureIndices =
        scaling.value("feature_indices", std::vector<int64_t>{});
    m_metadata.scalingMean = scaling.value("mean", std::vector<float>{});
    m_metadata.scalingStd = scaling.value("std", std::vector<float>{});
    if (m_metadata.scalingFeatureIndices.size() != m_metadata.scalingMean.size() ||
        m_metadata.scalingFeatureIndices.size() != m_metadata.scalingStd.size()) {
      error() << "Invalid input_scaling metadata: feature_indices, mean, and std "
              << "must have identical lengths" << endmsg;
      return StatusCode::FAILURE;
    }
  }

  const auto decayModes =
      metadata.value("decay_mode_class_to_hps", std::vector<int>{0, 1, 2, 10, 11, 15});
  if (decayModes.size() != kNumDecayModes) {
    error() << "Expected " << kNumDecayModes << " decay-mode labels, got "
            << decayModes.size() << endmsg;
    return StatusCode::FAILURE;
  }
  std::copy(decayModes.begin(), decayModes.end(), m_metadata.decayModeClassToHps.begin());
  return StatusCode::SUCCESS;
}

StatusCode mlTauAlg::initializeOnnx() {
  m_sessionOptions.SetIntraOpNumThreads(1);
  m_sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
  try {
    m_session = std::make_unique<Ort::Session>(
        m_onnxEnv, m_modelPath.value().c_str(), m_sessionOptions);
  } catch (const Ort::Exception& err) {
    error() << "Failed to initialize ONNXRuntime session: " << err.what() << endmsg;
    return StatusCode::FAILURE;
  }
  return StatusCode::SUCCESS;
}

StatusCode mlTauAlg::execute(const EventContext& /*ctx*/) const {
  const auto* jets = m_inputJets.get();
  const auto* primaryVertices = m_useLifetimeVariables ? m_inputPrimaryVertices.get() : nullptr;
  if (m_useLifetimeVariables) {
    (void)m_inputTracks.get();
  }
  auto* tauParticles = m_outputTaus.createAndPut();
  auto* tauIds = m_outputIds.createAndPut();

  const auto nJets = jets != nullptr ? jets->size() : 0;
  if (msgLevel(MSG::DEBUG)) {
    debug() << "Read " << nJets << " seed jets" << endmsg;
  }
  if (jets == nullptr || jets->empty()) {
    if (msgLevel(MSG::DEBUG)) {
      debug() << "Wrote 0 mlTau and 0 mlTauID entries" << endmsg;
    }
    return StatusCode::SUCCESS;
  }

  auto inputs = buildInputs(*jets, primaryVertices);
  dumpInputs(inputs);
  applyInputScaling(inputs);
  const auto outputs = runInference(inputs);
  fillOutputs(inputs, outputs, *tauParticles, *tauIds);
  if (msgLevel(MSG::DEBUG)) {
    debug() << "Wrote " << tauParticles->size() << " mlTau and "
            << tauIds->size() << " mlTauID entries" << endmsg;
  }
  return StatusCode::SUCCESS;
}

mlTauAlg::ModelInputs
mlTauAlg::buildInputs(const edm4hep::ReconstructedParticleCollection& jets,
                      const edm4hep::VertexCollection* primaryVertices) const {
  ModelInputs inputs;
  inputs.seedKinematics.reserve(jets.size());
  inputs.seedConstituents.reserve(jets.size());
  inputs.candFeatures.assign(jets.size() * kNumFeatures * kMaxCandidates, 0.0F);
  inputs.candKinematics.assign(jets.size() * kNumKinematics * kMaxCandidates, 0.0F);
  inputs.candMask.assign(jets.size() * kMaxCandidates, false);

  for (std::size_t jetIndex = 0; jetIndex < jets.size(); ++jetIndex) {
    const auto jet = jets[jetIndex];

    const auto jetMomentum = jet.getMomentum();
    const auto jetPt = ptFromMomentum(jetMomentum);
    const auto jetEta = etaFromMomentum(jetMomentum);
    const auto jetPhi = phiFromMomentum(jetMomentum);
    const auto jetEnergy = static_cast<float>(jet.getEnergy());
    const auto jetMass =
        massFromP4(jetMomentum.x, jetMomentum.y, jetMomentum.z, jetEnergy);
    inputs.seedKinematics.push_back({jetPt, jetEta, jetPhi, jetMass, jetEnergy});
    inputs.seedConstituents.emplace_back();

    const auto nConstituents =
        std::min<std::size_t>(jet.particles_size(), kMaxCandidates);
    inputs.seedConstituents.back().reserve(nConstituents);
    for (std::size_t candIndex = 0; candIndex < nConstituents; ++candIndex) {
      const auto cand = jet.getParticles(candIndex);
      inputs.seedConstituents.back().push_back(cand);
      const auto candMomentum = cand.getMomentum();
      const auto candPt = ptFromMomentum(candMomentum);
      const auto candEta = etaFromMomentum(candMomentum);
      const auto candPhi = phiFromMomentum(candMomentum);
      const auto candEnergy =
          energyFromMomentumAndMass(candMomentum.x, candMomentum.y, candMomentum.z,
                                    static_cast<float>(cand.getMass()));
      const auto absPdg = std::abs(cand.getPDG());
      const auto charge = static_cast<float>(cand.getCharge());
      const auto isHadron = isHadronPdg(absPdg);

      const auto featureOffset = jetIndex * kNumFeatures * kMaxCandidates + candIndex;
      auto setFeature = [&](std::size_t featureIndex, float value) {
        inputs.candFeatures[featureOffset + featureIndex * kMaxCandidates] = value;
      };

      setFeature(0, candEta - jetEta);
      setFeature(1, deltaPhi(candPhi, jetPhi));
      setFeature(2, safeLog(candPt));
      setFeature(3, safeLog(candEnergy));
      setFeature(4, safeLog(candPt / std::max(jetPt, 1.0e-6F)));
      setFeature(5, safeLog(candEnergy / std::max(jetEnergy, 1.0e-6F)));
      setFeature(6, std::hypot(candEta - jetEta, deltaPhi(candPhi, jetPhi)));
      setFeature(7, charge);
      setFeature(8, absPdg == 11 ? 1.0F : 0.0F);
      setFeature(9, absPdg == 13 ? 1.0F : 0.0F);
      setFeature(10, absPdg == 22 ? 1.0F : 0.0F);
      setFeature(11, isHadron && std::abs(charge) > 0.0F ? 1.0F : 0.0F);
      setFeature(12, isHadron && std::abs(charge) == 0.0F ? 1.0F : 0.0F);
      const auto lifetime = calculateLifetimeFeatures(cand, primaryVertices);
      setFeature(13, lifetime.dz);
      setFeature(14, lifetime.dzSignificance);
      setFeature(15, lifetime.dxy);
      setFeature(16, lifetime.dxySignificance);

      const auto kinOffset = jetIndex * kNumKinematics * kMaxCandidates + candIndex;
      inputs.candKinematics[kinOffset + 0 * kMaxCandidates] = candMomentum.x;
      inputs.candKinematics[kinOffset + 1 * kMaxCandidates] = candMomentum.y;
      inputs.candKinematics[kinOffset + 2 * kMaxCandidates] = candMomentum.z;
      inputs.candKinematics[kinOffset + 3 * kMaxCandidates] = candEnergy;
      inputs.candMask[jetIndex * kMaxCandidates + candIndex] = true;
    }
  }
  return inputs;
}

mlTauAlg::LifetimeFeatures mlTauAlg::calculateLifetimeFeatures(
    const edm4hep::ReconstructedParticle& candidate,
    const edm4hep::VertexCollection* primaryVertices) const {
  if (!m_useLifetimeVariables || primaryVertices == nullptr || primaryVertices->empty() ||
      candidate.tracks_size() == 0) {
    const auto invalid = m_invalidLifetimeValue.value();
    return {invalid, 1.0F, invalid, 1.0F, false};
  }

  const auto vertex = (*primaryVertices)[0].getPosition();
  const auto track = candidate.getTracks(0);
  if (track.trackStates_size() == 0) {
    const auto invalid = m_invalidLifetimeValue.value();
    return {invalid, 1.0F, invalid, 1.0F, false};
  }

  const auto state = track.getTrackStates(0);
  const auto d0 = static_cast<float>(state.D0);
  const auto z0 = static_cast<float>(state.Z0);
  const auto phi0 = static_cast<float>(state.phi);
  const auto tanLambda = static_cast<float>(state.tanLambda);
  const auto reference = state.referencePoint;

  const auto d0Error = std::sqrt(std::abs(static_cast<float>(state.covMatrix[0])));
  const auto phi0Error = std::sqrt(std::abs(static_cast<float>(state.covMatrix[2])));
  const auto z0Error = std::sqrt(std::abs(static_cast<float>(state.covMatrix[9])));
  const auto tanLambdaError = std::sqrt(std::abs(static_cast<float>(state.covMatrix[14])));

  const auto sinPhi = std::sin(phi0);
  const auto cosPhi = std::cos(phi0);
  const auto x0 = static_cast<float>(reference.x) + sinPhi * d0;
  const auto y0 = static_cast<float>(reference.y) - cosPhi * d0;
  const auto z0Prime = z0 + static_cast<float>(reference.z);

  const auto dx = cosPhi;
  const auto dy = sinPhi;
  const auto dzDirection = tanLambda;
  const auto numerator =
      -((x0 - vertex.x) * dx + (y0 - vertex.y) * dy + (z0Prime - vertex.z) * dzDirection);
  const auto denominator = dx * dx + dy * dy + dzDirection * dzDirection;
  const auto arcLength = safeRatio(numerator, denominator);

  const auto pcaX = arcLength * cosPhi + x0;
  const auto pcaY = arcLength * sinPhi + y0;
  const auto pcaZ = arcLength * tanLambda + z0Prime;

  const auto x0Error =
      std::sqrt(sinPhi * sinPhi * d0Error * d0Error +
                cosPhi * cosPhi * d0 * d0 * phi0Error * phi0Error);
  const auto y0Error =
      std::sqrt(cosPhi * cosPhi * d0Error * d0Error +
                sinPhi * sinPhi * d0 * d0 * phi0Error * phi0Error);
  const auto z0PrimeError = z0Error;

  const auto bxError = std::sqrt(sinPhi * sinPhi * phi0Error * phi0Error +
                                 x0Error * x0Error);
  const auto byError = std::sqrt(cosPhi * cosPhi * phi0Error * phi0Error +
                                 y0Error * y0Error);
  const auto bzError = std::sqrt(tanLambdaError * tanLambdaError +
                                 z0PrimeError * z0PrimeError);
  const auto numeratorError =
      std::sqrt(x0Error * x0Error * std::pow(2.0F * x0 - (cosPhi + x0) - vertex.x, 2.0F) +
                bxError * bxError * std::pow(vertex.x - (cosPhi + x0), 2.0F) +
                y0Error * y0Error * std::pow(2.0F * y0 - (sinPhi + y0) - vertex.y, 2.0F) +
                byError * byError * std::pow(vertex.y - (sinPhi + y0), 2.0F) +
                z0PrimeError * z0PrimeError *
                    std::pow(2.0F * z0Prime - (tanLambda + z0Prime) - vertex.z, 2.0F) +
                bzError * bzError * std::pow(vertex.z - (tanLambda + z0Prime), 2.0F));
  const auto denominatorError =
      std::sqrt((bxError * bxError + x0Error * x0Error) * std::pow(2.0F * dx, 2.0F) +
                (byError * byError + y0Error * y0Error) * std::pow(2.0F * dy, 2.0F) +
                (bzError * bzError + z0PrimeError * z0PrimeError) *
                    std::pow(2.0F * dzDirection, 2.0F));
  const auto arcLengthError =
      std::sqrt(std::pow(safeRatio(numeratorError, denominator), 2.0F) +
                std::pow(safeRatio(numerator * denominatorError,
                                   denominator * denominator), 2.0F));

  const auto pcaXError =
      std::sqrt(std::pow(cosPhi * arcLengthError, 2.0F) + x0Error * x0Error +
                std::pow(arcLength * sinPhi * phi0Error, 2.0F));
  const auto pcaYError =
      std::sqrt(std::pow(sinPhi * arcLengthError, 2.0F) + y0Error * y0Error +
                std::pow(arcLength * cosPhi * phi0Error, 2.0F));
  const auto pcaZError =
      std::sqrt(std::pow(tanLambda * arcLengthError, 2.0F) +
                std::pow(arcLength * tanLambdaError, 2.0F) +
                z0PrimeError * z0PrimeError);

  const auto deltaX = vertex.x - pcaX;
  const auto deltaY = vertex.y - pcaY;
  const auto deltaZ = pcaZ - vertex.z;
  const auto dxy = std::sqrt(deltaX * deltaX + deltaY * deltaY);
  const auto dz = std::abs(deltaZ);
  const auto dxyError = dxy > 1.0e-10F
                            ? std::sqrt(std::pow(deltaX * pcaXError, 2.0F) +
                                        std::pow(deltaY * pcaYError, 2.0F)) /
                                  dxy
                            : 0.0F;
  const auto dzError = pcaZError;

  return {dz, safeRatio(dz, dzError), dxy, safeRatio(dxy, dxyError), true};
}

void mlTauAlg::dumpInputs(const ModelInputs& inputs) const {
  if (m_inputDumpPath.value().empty()) {
    return;
  }

  std::ofstream output;
  if (m_inputDumpEvent == 0) {
    output.open(m_inputDumpPath.value(), std::ios::out | std::ios::trunc);
  } else {
    output.open(m_inputDumpPath.value(), std::ios::out | std::ios::app);
  }
  if (!output) {
    warning() << "Could not open input tensor dump path: " << m_inputDumpPath << endmsg;
    ++m_inputDumpEvent;
    return;
  }

  nlohmann::json record;
  record["event"] = m_inputDumpEvent;
  record["n_seeds"] = inputs.seedKinematics.size();
  record["seed_kinematics"] = nlohmann::json::array();
  for (const auto& seed : inputs.seedKinematics) {
    record["seed_kinematics"].push_back(
        {{"pt", seed.pt}, {"eta", seed.eta}, {"phi", seed.phi},
         {"mass", seed.mass}, {"energy", seed.energy}});
  }

  const auto nSeeds = inputs.seedKinematics.size();
  record["cand_features"] = nlohmann::json::array();
  record["cand_kinematics"] = nlohmann::json::array();
  record["cand_mask"] = nlohmann::json::array();
  for (std::size_t seedIndex = 0; seedIndex < nSeeds; ++seedIndex) {
    nlohmann::json seedFeatures = nlohmann::json::array();
    for (std::size_t featureIndex = 0; featureIndex < kNumFeatures; ++featureIndex) {
      nlohmann::json featureValues = nlohmann::json::array();
      for (std::size_t candIndex = 0; candIndex < kMaxCandidates; ++candIndex) {
        featureValues.push_back(inputs.candFeatures[seedIndex * kNumFeatures * kMaxCandidates +
                                                    featureIndex * kMaxCandidates + candIndex]);
      }
      seedFeatures.push_back(featureValues);
    }
    record["cand_features"].push_back(seedFeatures);

    nlohmann::json seedKinematics = nlohmann::json::array();
    for (std::size_t kinIndex = 0; kinIndex < kNumKinematics; ++kinIndex) {
      nlohmann::json kinValues = nlohmann::json::array();
      for (std::size_t candIndex = 0; candIndex < kMaxCandidates; ++candIndex) {
        kinValues.push_back(inputs.candKinematics[seedIndex * kNumKinematics * kMaxCandidates +
                                                  kinIndex * kMaxCandidates + candIndex]);
      }
      seedKinematics.push_back(kinValues);
    }
    record["cand_kinematics"].push_back(seedKinematics);

    nlohmann::json maskValues = nlohmann::json::array();
    for (std::size_t candIndex = 0; candIndex < kMaxCandidates; ++candIndex) {
      maskValues.push_back(inputs.candMask[seedIndex * kMaxCandidates + candIndex]);
    }
    record["cand_mask"].push_back(maskValues);
  }

  output << record.dump() << '\n';
  ++m_inputDumpEvent;
}

void mlTauAlg::applyInputScaling(ModelInputs& inputs) const {
  if (!m_metadata.inputScalingEnabled) {
    return;
  }

  const auto nJets = inputs.seedKinematics.size();
  for (std::size_t scaleIndex = 0; scaleIndex < m_metadata.scalingFeatureIndices.size();
       ++scaleIndex) {
    const auto featureIndex = static_cast<std::size_t>(m_metadata.scalingFeatureIndices[scaleIndex]);
    const auto mean = m_metadata.scalingMean[scaleIndex];
    const auto std = m_metadata.scalingStd[scaleIndex];
    for (std::size_t jetIndex = 0; jetIndex < nJets; ++jetIndex) {
      for (std::size_t candIndex = 0; candIndex < kMaxCandidates; ++candIndex) {
        if (!inputs.candMask[jetIndex * kMaxCandidates + candIndex]) {
          continue;
        }
        auto& value = inputs.candFeatures[jetIndex * kNumFeatures * kMaxCandidates +
                                          featureIndex * kMaxCandidates + candIndex];
        value = (value - mean) / std;
      }
    }
  }
}

std::vector<Ort::Value> mlTauAlg::runInference(const ModelInputs& inputs) const {
  if (inputs.seedKinematics.empty()) {
    return {};
  }

  Ort::MemoryInfo memoryInfo =
      Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator,
                                 OrtMemType::OrtMemTypeDefault);
  const int64_t batch = static_cast<int64_t>(inputs.seedKinematics.size());
  std::array<int64_t, 3> featureShape{batch, static_cast<int64_t>(kNumFeatures),
                                      static_cast<int64_t>(kMaxCandidates)};
  std::array<int64_t, 3> kinematicsShape{batch, static_cast<int64_t>(kNumKinematics),
                                         static_cast<int64_t>(kMaxCandidates)};
  std::array<int64_t, 3> maskShape{batch, 1, static_cast<int64_t>(kMaxCandidates)};

  std::vector<uint8_t> maskBytes(inputs.candMask.begin(), inputs.candMask.end());
  std::vector<Ort::Value> onnxInputs;
  onnxInputs.emplace_back(Ort::Value::CreateTensor<float>(
      memoryInfo, const_cast<float*>(inputs.candFeatures.data()),
      inputs.candFeatures.size(), featureShape.data(), featureShape.size()));
  onnxInputs.emplace_back(Ort::Value::CreateTensor<float>(
      memoryInfo, const_cast<float*>(inputs.candKinematics.data()),
      inputs.candKinematics.size(), kinematicsShape.data(), kinematicsShape.size()));
  onnxInputs.emplace_back(Ort::Value::CreateTensor<bool>(
      memoryInfo, reinterpret_cast<bool*>(maskBytes.data()), maskBytes.size(),
      maskShape.data(), maskShape.size()));

  return m_session->Run(Ort::RunOptions{nullptr}, m_inputNames.data(),
                        onnxInputs.data(), onnxInputs.size(), m_outputNames.data(),
                        m_outputNames.size());
}

void mlTauAlg::fillOutputs(const ModelInputs& inputs,
                           const std::vector<Ort::Value>& outputs,
                           edm4hep::ReconstructedParticleCollection& tauParticles,
                           edm4hep::ParticleIDCollection& tauIds) const {
  if (inputs.seedKinematics.empty() || outputs.empty()) {
    return;
  }

  const auto* tauIdLogits = outputs[0].GetTensorData<float>();
  const auto* chargeLogit = outputs[1].GetTensorData<float>();
  const auto* decayModeLogits = outputs[2].GetTensorData<float>();
  const auto* kinematics = outputs[3].GetTensorData<float>();

  for (std::size_t jetIndex = 0; jetIndex < inputs.seedKinematics.size(); ++jetIndex) {
    const auto tauScore = softmaxSignalScore(tauIdLogits + jetIndex * 2);
    if (tauScore < m_tauScoreCut) {
      continue;
    }

    const auto& seed = inputs.seedKinematics[jetIndex];

    const auto* kin = kinematics + jetIndex * 5;
    const auto tauPt = seed.pt * std::exp(kin[0]);
    const auto tauEta = seed.eta + kin[1];
    const auto tauPhi = wrapPhi(seed.phi + std::atan2(kin[2], kin[3]));
    const auto tauMass = seed.mass * std::exp(kin[4]);
    const auto tauPx = tauPt * std::cos(tauPhi);
    const auto tauPy = tauPt * std::sin(tauPhi);
    const auto tauPz = tauPt * std::sinh(tauEta);
    const auto tauEnergy =
        std::sqrt(std::max(tauPt * tauPt * std::cosh(tauEta) * std::cosh(tauEta) +
                              tauMass * tauMass,
                          0.0F));

    auto tau = tauParticles.create();
    tau.setMomentum({tauPx, tauPy, tauPz});
    tau.setEnergy(tauEnergy);
    tau.setMass(tauMass);
    tau.setCharge(sigmoid(chargeLogit[jetIndex]) >= 0.5F ? 1.0F : -1.0F);
    tau.setPDG(static_cast<int>(tau.getCharge() > 0 ? -15 : 15));
    tau.setGoodnessOfPID(tauScore);
    for (const auto& constituent : inputs.seedConstituents[jetIndex]) {
      tau.addToParticles(constituent);
    }

    const auto dmClass =
        argmax(decayModeLogits + jetIndex * kNumDecayModes, kNumDecayModes);
    const auto dmProbabilities = softmax(decayModeLogits + jetIndex * kNumDecayModes,
                                         kNumDecayModes);
    auto pid = tauIds.create();
    pid.setType(m_metadata.decayModeClassToHps[dmClass]);
    pid.setPDG(static_cast<int>(tau.getCharge() > 0 ? -15 : 15));
    pid.setLikelihood(tauScore);
    pid.setAlgorithmType(15);
    pid.setParticle(tau);
    pid.addToParameters(sigmoid(chargeLogit[jetIndex]));
    for (std::size_t dmIndex = 0; dmIndex < kNumDecayModes; ++dmIndex) {
      pid.addToParameters(dmProbabilities[dmIndex]);
    }
  }
}

auto mlTauAlg::softmax(const float* logits, std::size_t size)
    -> std::array<float, kNumDecayModes> {
  std::array<float, kNumDecayModes> probabilities{};
  const auto maxLogit = *std::max_element(logits, logits + size);
  float sum = 0.0F;
  for (std::size_t index = 0; index < size; ++index) {
    probabilities[index] = std::exp(logits[index] - maxLogit);
    sum += probabilities[index];
  }
  for (std::size_t index = 0; index < size; ++index) {
    probabilities[index] /= sum;
  }
  return probabilities;
}

float mlTauAlg::softmaxSignalScore(const float* logits) {
  const auto maxLogit = std::max(logits[0], logits[1]);
  const auto bkg = std::exp(logits[0] - maxLogit);
  const auto sig = std::exp(logits[1] - maxLogit);
  return sig / (bkg + sig);
}

float mlTauAlg::sigmoid(float value) {
  return 1.0F / (1.0F + std::exp(-value));
}

int mlTauAlg::argmax(const float* values, std::size_t size) {
  return static_cast<int>(
      std::distance(values, std::max_element(values, values + size)));
}

float mlTauAlg::wrapPhi(float phi) {
  return std::atan2(std::sin(phi), std::cos(phi));
}

} // namespace mlTau
