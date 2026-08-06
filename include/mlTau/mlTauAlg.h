#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "Gaudi/Algorithm.h"
#include "Gaudi/Property.h"
#include "GaudiKernel/EventContext.h"
#include "k4FWCore/DataHandle.h"

#include "edm4hep/ParticleIDCollection.h"
#include "edm4hep/ReconstructedParticleCollection.h"
#include "edm4hep/Track.h"
#include "edm4hep/TrackCollection.h"
#include "edm4hep/VertexCollection.h"

#include "onnxruntime_cxx_api.h"

namespace mlTau {

class mlTauAlg final : public Gaudi::Algorithm {
public:
  mlTauAlg(const std::string& name, ISvcLocator* svcLoc);

  StatusCode initialize() override;
  StatusCode execute(const EventContext& ctx) const override;

private:
  static constexpr std::size_t kNumFeatures = 17;
  static constexpr std::size_t kNumKinematics = 4;
  static constexpr std::size_t kMaxCandidates = 20;
  static constexpr std::size_t kNumDecayModes = 6;

  struct ModelMetadata {
    bool inputScalingEnabled = false;
    std::vector<int64_t> scalingFeatureIndices;
    std::vector<float> scalingMean;
    std::vector<float> scalingStd;
    std::array<int, kNumDecayModes> decayModeClassToHps{0, 1, 2, 10, 11, 15};
  };

  struct SeedKinematics {
    float pt = 0.0F;
    float eta = 0.0F;
    float phi = 0.0F;
    float mass = 0.0F;
    float energy = 0.0F;
  };

  struct LifetimeFeatures {
    float dz = 0.0F;
    float dzSignificance = 0.0F;
    float dxy = 0.0F;
    float dxySignificance = 0.0F;
    bool valid = false;
  };

  struct ModelInputs {
    std::vector<float> candFeatures;
    std::vector<float> candKinematics;
    std::vector<bool> candMask;
    std::vector<SeedKinematics> seedKinematics;
    std::vector<std::vector<edm4hep::ReconstructedParticle>> seedConstituents;
  };

  StatusCode loadMetadata();
  StatusCode initializeOnnx();

  ModelInputs buildInputs(const edm4hep::ReconstructedParticleCollection& jets,
                          const edm4hep::VertexCollection* primaryVertices) const;
  void applyInputScaling(ModelInputs& inputs) const;
  std::vector<Ort::Value> runInference(const ModelInputs& inputs) const;
  void dumpInputs(const ModelInputs& inputs) const;
  LifetimeFeatures calculateLifetimeFeatures(const edm4hep::ReconstructedParticle& candidate,
                                             const edm4hep::VertexCollection* primaryVertices) const;

  void fillOutputs(const ModelInputs& inputs,
                   const std::vector<Ort::Value>& outputs,
                   edm4hep::ReconstructedParticleCollection& tauParticles,
                   edm4hep::ParticleIDCollection& tauIds) const;

  static float softmaxSignalScore(const float* logits);
  static std::array<float, kNumDecayModes> softmax(const float* logits, std::size_t size);
  static float sigmoid(float value);
  static int argmax(const float* values, std::size_t size);
  static float wrapPhi(float phi);

  Gaudi::Property<std::string> m_modelPath{
      this, "ModelPath", "model.onnx", "Path to the mlTau ONNX model"};
  Gaudi::Property<std::string> m_metadataPath{
      this, "MetadataPath", "metadata.json", "Path to the mlTau metadata JSON"};
  Gaudi::Property<float> m_tauScoreCut{
      this, "TauScoreCut", 0.0F, "Minimum tau score for writing an output tau"};
  Gaudi::Property<bool> m_useLifetimeVariables{
      this, "UseLifetimeVariables", true,
      "Fill cand_dz/cand_dxy features from candidate tracks and primary vertices"};
  Gaudi::Property<float> m_invalidLifetimeValue{
      this, "InvalidLifetimeValue", -1000.0F,
      "Value used for missing lifetime variables, matching the training ntuplizer"};
  Gaudi::Property<std::string> m_inputDumpPath{
      this, "InputDumpPath", "",
      "Optional JSONL path for dumping unscaled ONNX input tensors for validation"};

  mutable k4FWCore::DataHandle<edm4hep::ReconstructedParticleCollection> m_inputJets{
      "InputJets", Gaudi::DataHandle::Reader, this};
  mutable k4FWCore::DataHandle<edm4hep::TrackCollection> m_inputTracks{
      "SiTracks_Refitted", Gaudi::DataHandle::Reader, this};
  mutable k4FWCore::DataHandle<edm4hep::VertexCollection> m_inputPrimaryVertices{
      "PrimaryVertices", Gaudi::DataHandle::Reader, this};
  mutable k4FWCore::DataHandle<edm4hep::ReconstructedParticleCollection> m_outputTaus{
      "mlTau", Gaudi::DataHandle::Writer, this};
  mutable k4FWCore::DataHandle<edm4hep::ParticleIDCollection> m_outputIds{
      "mlTauID", Gaudi::DataHandle::Writer, this};

  ModelMetadata m_metadata;
  Ort::Env m_onnxEnv{ORT_LOGGING_LEVEL_WARNING, "mlTau"};
  Ort::SessionOptions m_sessionOptions;
  std::unique_ptr<Ort::Session> m_session;
  mutable std::size_t m_inputDumpEvent = 0;
  const std::vector<const char*> m_inputNames{"cand_features", "cand_kinematics", "cand_mask"};
  const std::vector<const char*> m_outputNames{
      "is_tau_logits", "charge_logit", "decay_mode_logits", "kinematics"};
};

} // namespace mlTau
