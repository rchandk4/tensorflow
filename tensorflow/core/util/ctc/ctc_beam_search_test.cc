/* Copyright 2016 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This test illustrates how to make use of the CTCBeamSearchDecoder using a
// custom BeamScorer and BeamState based on a dictionary with a few artificial
// words.
#include "tensorflow/core/util/ctc/ctc_beam_search.h"

#include <cmath>
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/test.h"

namespace {

typedef std::vector<std::vector<std::vector<float>>> TestData;
using tensorflow::ctc::CTCBeamSearchDecoder;
using tensorflow::ctc::CTCDecoder;

// The HistoryBeamState is used to keep track of the current candidate and
// caches the expansion score (needed by the scorer).
struct HistoryBeamState {
  float score;
  std::vector<int> labels;
};

// DictionaryBeamScorer essentially favors candidates that can still become
// dictionary words. As soon as a beam candidate is not a dictionary word or
// a prefix of a dictionary word it gets a low probability at each step.
//
// The dictionary itself is hard-coded a static const variable of the class.
class DictionaryBeamScorer
    : public tensorflow::ctc::BeamScorerInterface<HistoryBeamState> {
 public:
  void InitializeState(HistoryBeamState* root) const override {
    root->score = 0;
  }

  void ExpandState(const HistoryBeamState& from_state, int from_label,
                   HistoryBeamState* to_state, int to_label) const override {
    // Keep track of the current complete candidate by storing the labels along
    // the expansion path in the beam state.
    to_state->labels.push_back(to_label);
    SetStateScoreAccordingToDict(to_state);
  }

  void ExpandStateEnd(HistoryBeamState* state) const override {
    SetStateScoreAccordingToDict(state);
  }

  float GetStateExpansionScore(const HistoryBeamState& state,
                               float previous_score) const override {
    return previous_score + state.score;
  }

  float GetStateEndExpansionScore(
      const HistoryBeamState& state) const override {
    return state.score;
  }

  // Simple dictionary used when scoring the beams to check if they are prefixes
  // of dictionary words (see SetStateScoreAccordingToDict below).
  static const std::vector<std::vector<int>> dictionary_;

 private:
  void SetStateScoreAccordingToDict(HistoryBeamState* state) const;
};

const std::vector<std::vector<int>> DictionaryBeamScorer::dictionary_ = {
    {3}, {3, 1}};

void DictionaryBeamScorer::SetStateScoreAccordingToDict(
    HistoryBeamState* state) const {
  // Check if the beam can still be a dictionary word (e.g. prefix of one).
  const std::vector<int>& candidate = state->labels;
  for (int w = 0; w < dictionary_.size(); ++w) {
    const std::vector<int>& word = dictionary_[w];
    // If the length of the current beam is already larger, skip.
    if (candidate.size() > word.size()) {
      continue;
    }
    if (std::equal(word.begin(), word.begin() + candidate.size(),
                   candidate.begin())) {
      state->score = std::log(1.0);
      return;
    }
  }
  // At this point, the candidate certainly can't be in the dictionary.
  state->score = std::log(0.01);
}

TEST(CtcBeamSearch, DecodingWithAndWithoutDictionary) {
  const int batch_size = 1;
  const int timesteps = 5;
  const int top_paths = 3;
  const int num_classes = 6;

  // Plain decoder using hibernating beam search algorithm.
  CTCBeamSearchDecoder<> decoder(num_classes, 10 * top_paths, batch_size,
                                 false);

  // Dictionary decoder, allowing only two dictionary words : {3}, {3, 1}.
  CTCBeamSearchDecoder<HistoryBeamState, DictionaryBeamScorer>
      dictionary_decoder(num_classes, top_paths, batch_size, false);

  // Raw data containers (arrays of floats, ints, etc.).
  int sequence_lengths[batch_size] = {timesteps};
  float input_data_mat[timesteps][batch_size][num_classes] = {
      {{0, 0.6, 0, 0.4, 0, 0}},
      {{0, 0.5, 0, 0.5, 0, 0}},
      {{0, 0.4, 0, 0.6, 0, 0}},
      {{0, 0.4, 0, 0.6, 0, 0}},
      {{0, 0.4, 0, 0.6, 0, 0}}};

  // The CTCDecoder works with log-probs.
  for (int t = 0; t < timesteps; ++t) {
    for (int b = 0; b < batch_size; ++b) {
      for (int c = 0; c < num_classes; ++c) {
        input_data_mat[t][b][c] = std::log(input_data_mat[t][b][c]);
      }
    }
  }

  // Plain output, without any additional scoring.
  std::vector<CTCDecoder::Output> expected_output = {
      {{1, 3}, {1, 3, 1}, {3, 1, 3}},
  };

  // Dictionary outputs: preference for dictionary candidates. The
  // second-candidate is there, despite it not being a dictionary word, due to
  // stronger probability in the input to the decoder.
  std::vector<CTCDecoder::Output> expected_dict_output = {
      {{3}, {1, 3}, {3, 1}},
  };

  // Convert data containers to the formatat accepted by the decoder, simply
  // mapping the memory from the container to an Eigen::ArrayXi,::MatrixXf,
  // using Eigen::Map.
  Eigen::Map<const Eigen::ArrayXi> seq_len(&sequence_lengths[0], batch_size);
  std::vector<Eigen::Map<const Eigen::MatrixXf>> inputs;
  for (int t = 0; t < timesteps; ++t) {
    inputs.emplace_back(&input_data_mat[t][0][0], batch_size, num_classes);
  }

  // Prepare containers for output and scores.
  std::vector<CTCDecoder::Output> outputs(top_paths);
  for (CTCDecoder::Output& output : outputs) {
    output.resize(batch_size);
  }
  float score[batch_size][top_paths] = {{0.0}};
  Eigen::Map<Eigen::MatrixXf> scores(&score[0][0], batch_size, top_paths);

  decoder.Decode(seq_len, inputs, &outputs, &scores);
  for (int path = 0; path < top_paths; ++path) {
    EXPECT_EQ(outputs[path][0], expected_output[0][path]);
  }

  // Prepare dictionary outputs.
  std::vector<CTCDecoder::Output> dict_outputs(top_paths);
  for (CTCDecoder::Output& output : dict_outputs) {
    output.resize(batch_size);
  }
  dictionary_decoder.Decode(seq_len, inputs, &dict_outputs, &scores);
  for (int path = 0; path < top_paths; ++path) {
    EXPECT_EQ(dict_outputs[path][0], expected_dict_output[0][path]);
  }
}

}  // namespace
