// Copyright 2016 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.!

#include "sentencepiece_processor.h"

#include <map>
#include <set>
#include <utility>

#include "common.h"
#include "filesystem.h"
#include "model_factory.h"
#include "model_interface.h"
#include "normalizer.h"
#include "sentencepiece.pb.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/strings/numbers.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_join.h"
#include "third_party/absl/strings/str_replace.h"
#include "third_party/absl/strings/str_split.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/absl/strings/strip.h"
#include "unigram_model.h"
#include "util.h"

namespace sentencepiece {
namespace {

// Replaces white space with U+2581 (LOWER ONE EIGHT BLOCK).
const char kSpaceSymbol[] = "\xe2\x96\x81";

// Encodes <unk> into U+2047 (DOUBLE QUESTION MARK),
// since this character can be useful both for user and
// developer. We can easily figure out that <unk> is emitted.
const char kDefaultUnknownSymbol[] = " \xE2\x81\x87 ";
}  // namespace

SentencePieceProcessor::SentencePieceProcessor() {}
SentencePieceProcessor::~SentencePieceProcessor() {}

util::Status SentencePieceProcessor::Load(absl::string_view filename) {
  auto model_proto = absl::make_unique<ModelProto>();
  RETURN_IF_ERROR(io::LoadModelProto(filename, model_proto.get()));
  return Load(std::move(model_proto));
}

void SentencePieceProcessor::LoadOrDie(absl::string_view filename) {
  CHECK_OK(Load(filename));
}

util::Status SentencePieceProcessor::Load(const ModelProto &model_proto) {
  auto model_proto_copy = absl::make_unique<ModelProto>();
  *model_proto_copy = model_proto;
  return Load(std::move(model_proto_copy));
}

util::Status SentencePieceProcessor::LoadFromSerializedProto(
    absl::string_view serialized) {
  auto model_proto = absl::make_unique<ModelProto>();
  CHECK_OR_RETURN(
      model_proto->ParseFromArray(serialized.data(), serialized.size()));
  return Load(std::move(model_proto));
}

util::Status SentencePieceProcessor::Load(
    std::unique_ptr<ModelProto> model_proto) {
  model_proto_ = std::move(model_proto);
  model_ = ModelFactory::Create(*model_proto_);

  normalizer_ = absl::make_unique<normalizer::Normalizer>(
      model_proto_->normalizer_spec(), model_proto_->trainer_spec());

  if (model_proto_->has_denormalizer_spec() &&
      !model_proto_->denormalizer_spec().precompiled_charsmap().empty()) {
    denormalizer_ = absl::make_unique<normalizer::Normalizer>(
        model_proto_->denormalizer_spec());
  }

  // Escapes user-defined-symbols in normalizer.
  normalizer_->SetPrefixMatcher(model_->prefix_matcher());

  RETURN_IF_ERROR(status());

  // Running self-testing.
  std::vector<std::string> errors, sps;
  for (const auto &s : model_proto_->self_test_data().samples()) {
    RETURN_IF_ERROR(Encode(s.input(), &sps));
    const std::string result = absl::StrJoin(sps, " ");
    if (!model_->VerifyOutputsEquivalent(s.expected(), result)) {
      errors.emplace_back(
          absl::StrCat(s.input(), "\t", s.expected(), "\t", result));
    }
  }

  if (!errors.empty()) {
    LOG(INFO) << errors.size() << "/"
              << model_proto_->self_test_data().samples_size()
              << " samples did not pass the test.";
    for (const auto &e : errors) {
      LOG(INFO) << e;
    }
    return util::InternalError("Self-test failures. See LOG(INFO).");
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::SetEncoderVersion(
    EncoderVersion encoder_version) {
  return model_->SetEncoderVersion(encoder_version);
}

EncoderVersion SentencePieceProcessor::GetEncoderVersion() const {
  return model_->GetEncoderVersion();
}

util::Status SentencePieceProcessor::SetEncodeExtraOptions(
    absl::string_view extra_options) {
  return ParseExtraOptions(extra_options, &encode_extra_options_);
}

util::Status SentencePieceProcessor::SetDecodeExtraOptions(
    absl::string_view extra_options) {
  return ParseExtraOptions(extra_options, &decode_extra_options_);
}

util::Status SentencePieceProcessor::status() const {
  CHECK_OR_RETURN(model_) << "Model is not initialized.";
  CHECK_OR_RETURN(normalizer_) << "Normalizer is not initialized.";
  RETURN_IF_ERROR(model_->status());
  RETURN_IF_ERROR(normalizer_->status());
  return util::OkStatus();
}

util::Status SentencePieceProcessor::SetVocabulary(
    const std::vector<std::string> &valid_vocab) {
  RETURN_IF_ERROR(status());

  // TODO(taku): supports vocabulary constraint in BPE model.
  const auto type = model_proto_->trainer_spec().model_type();
  CHECK_OR_RETURN(type == TrainerSpec::UNIGRAM || type == TrainerSpec::BPE)
      << "Vocabulary constraint is only enabled in subword units.";

  const std::set<std::string> vocab(valid_vocab.begin(), valid_vocab.end());

  for (int i = 0; i < model_proto_->pieces_size(); ++i) {
    auto *piece = model_proto_->mutable_pieces(i);
    if (piece->type() == ModelProto::SentencePiece::CONTROL ||
        piece->type() == ModelProto::SentencePiece::UNKNOWN ||
        piece->type() == ModelProto::SentencePiece::USER_DEFINED) {
      continue;
    }
    if (vocab.find(piece->piece()) != vocab.end() ||
        string_util::OneCharLen(piece->piece().c_str()) ==
            piece->piece().size()) {
      piece->set_type(ModelProto::SentencePiece::NORMAL);
    } else {
      piece->set_type(ModelProto::SentencePiece::UNUSED);
    }
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::ResetVocabulary() {
  RETURN_IF_ERROR(status());
  for (auto &piece : *(model_proto_->mutable_pieces())) {
    if (piece.type() == ModelProto::SentencePiece::UNUSED)
      piece.set_type(ModelProto::SentencePiece::NORMAL);
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::LoadVocabulary(absl::string_view filename,
                                                    int threshold) {
  auto input = filesystem::NewReadableFile(filename);
  RETURN_IF_ERROR(input->status());

  std::string line;
  std::vector<std::string> vocab;

  while (input->ReadLine(&line)) {
    const std::vector<std::string> v = absl::StrSplit(line, "\t");
    CHECK_GE_OR_RETURN(v.size(), 1);
    CHECK_OR_RETURN(!v[0].empty());
    int32 freq = 1;
    if (v.size() >= 2) {
      CHECK_OR_RETURN(absl::SimpleAtoi(v[1], &freq))
          << "Could not parse the frequency";
    }
    if (freq >= threshold) {
      vocab.emplace_back(v[0]);
    }
  }

  return SetVocabulary(vocab);
}

#define CHECK_OR_RETURN_STATUS_STL(container)               \
  RETURN_IF_ERROR(status());                                \
  CHECK_OR_RETURN(container) << "output container is null"; \
  container->clear();

#define CHECK_OR_RETURN_STATUS_PROTO(proto)         \
  RETURN_IF_ERROR(status());                        \
  CHECK_OR_RETURN(proto) << "output proto is null"; \
  proto->Clear();

//////////////////////////////////////////////////////////////
// Simple API.
//////////////////////////////////////////////////////////////
std::vector<int> extract_each_digit(int x)
{
    std::vector<int> digits;
    while(x>0)
    {
      digits.push_back(x%10);
      x/=10;
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

void rle(std::vector<std::string> pieces, std::vector<std::string> *tokens){
        int n = pieces.size();

        for (int i = 0; i < n; i++) {
    
            int count = 1;
            while (i < n - 1 && pieces[i] == pieces[i + 1]) {
                count++;
                i++;
            }
            // Word and its Count
            if (count > 1){

              tokens->emplace_back(pieces[i]);
              tokens->emplace_back("(#startrepeat)");
              
              //tokens->emplace_back("xx");
              
              std::vector<int> digits = extract_each_digit(count) ;
              for (auto d: digits){
                // Guarantee that these are digit symbols
                tokens->emplace_back(std::to_string(d)) ;
              }

              tokens->emplace_back("(#endrepeat)");

            }
            else{
              // 
              tokens->emplace_back(pieces[i]);
            }
            
        }

    }

int VectorToInt(std::vector<int> v)
{
    std::reverse(v.begin(), v.end());
    int decimal = 1;
    int total = 0;

    for (auto& it : v)
    {
        total += it * decimal;
        decimal *= 10;
    }
    
    return total;
}

std::vector<std::size_t> locate_all( const std::vector<absl::string_view>& seq, const std::string& what )
{
    std::vector<std::size_t> result ;
    for( std::size_t i = 0 ; i < seq.size() ; ++i ) if( seq[i] == what ) result.push_back(i) ;
    return result ;
}

std::vector<std::string> _expand_from_pieces(std::vector<std::string> pieces){

  //std::cout<<pieces<<std::endl;

  // Just look for the control symbol
  auto it1 = std::find(pieces.begin(), pieces.end(), "(#startrepeat)");
  //std::cout<<"----------------";

  auto it2 = std::find(pieces.begin(), pieces.end(), "(#endrepeat)");
  int start_position = it1 - pieces.begin();
  int end_position = it2 - pieces.begin();
  std::string repeat_token = pieces[start_position-1];

  //std::cout<<repeat_token<<std::endl;
  std::vector<int> num;

  if (it1!= pieces.end() and it2!= pieces.end()){

    int j = start_position + 1;
    while (j!=end_position){
      num.push_back(std::stoi(std::string(pieces[j])));
      j++;
    }
    int count = VectorToInt(num) - 1;
    //std::cout<<count<<std::endl;
    pieces.erase(it1, it2+1);
    pieces.insert(it1, count, repeat_token);
  }
  return pieces;
}

std::vector<std::string> expand_from_pieces(std::vector<std::string> pieces){
  while (std::find(pieces.begin(), pieces.end(), "(#startrepeat)") != pieces.end()){
    pieces = _expand_from_pieces(pieces);
  }
  return pieces;
}

std::vector<int> SentencePieceProcessor::_expand_from_ids(const std::vector<int> ids) const {

  // Just look for the control symbol
  auto it1 = std::find(ids.begin(), ids.end(), model_->PieceToId("(#startrepeat)"));
  int start_position = it1 - ids.begin();
  auto it2 = std::find(ids.begin(), ids.end(), model_->PieceToId("(#endrepeat)"));
  int end_position = it2 - ids.begin();

  // // Erase the End Token
  // ids.erase( it2 );

  std::vector<int> num;
  std::vector<int> new_ids;

  int p = 0;
  while (p != start_position){
    new_ids.push_back(ids[p]);
    p++;
  }

  // Get the Symbol
  int symbol = *(it1-1);
  it1 += 1;

  // Get the Count
  while (it1 != it2){
    num.push_back( std::stoi(model_->IdToPiece(*it1)) );
    it1 += 1;
  }
  int count = VectorToInt(num);
  //std::cout<<num<<std::endl;

  // Update the IDs
  int i = 0;
  while (i < count){
    new_ids.push_back(symbol);
    i++;
  }

  // Append the Suffix
  int s = end_position+1;
  while(s != ids.size()){
    new_ids.push_back(ids[s]);
    s++;
  }
  
  return new_ids;
}

std::vector<int> SentencePieceProcessor::expand_from_ids(const std::vector<int> ids) const {

  std::vector<int> new_ids(ids);
  while (std::find(new_ids.begin(), new_ids.end(), model_->PieceToId("(#startrepeat)")) != new_ids.end()){
    new_ids = _expand_from_ids(new_ids);
  }
  return new_ids;
}

/////////////////////////////////////////////////////////////
// Simple API.
util::Status SentencePieceProcessor::Encode(
    absl::string_view input, std::vector<std::string> *pieces) const {
  CHECK_OR_RETURN_STATUS_STL(pieces);

  // std::cout<<"Encode 1"<<std::endl;
  // std::cout<<input<<std::endl;
  // std::cout<<"Encode 1"<<std::endl;

  SentencePieceText spt;
  RETURN_IF_ERROR(Encode(input, &spt));

  std::vector<std::string> temp_pieces;

  for (const auto &sp : spt.pieces()) {
    temp_pieces.push_back(sp.piece());
  }

  rle(temp_pieces, pieces);

  return util::OkStatus();
}

util::Status SentencePieceProcessor::Encode(absl::string_view input,
                                            std::vector<int> *ids) const {
  CHECK_OR_RETURN_STATUS_STL(ids);

  // std::cout<<"Encode 2"<<std::endl;
  // std::cout<<input<<std::endl;
  // std::cout<<"Encode 2"<<std::endl;

  SentencePieceText spt;

  RETURN_IF_ERROR(Encode(input, &spt));
  std::vector<std::string> temp_pieces;
  for (const auto &sp : spt.pieces()) {
    temp_pieces.push_back(sp.piece());
  }

  std::vector<std::string> pieces;
  rle(temp_pieces, &pieces);
  for (const auto piece: pieces) {
   
    ids->emplace_back(model_->PieceToId(piece));
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::Decode(
    const std::vector<std::string> &pieces, std::string *detokenized) const {
  CHECK_OR_RETURN_STATUS_STL(detokenized);

  auto new_pieces = expand_from_pieces(pieces);

  SentencePieceText spt;
  RETURN_IF_ERROR(Decode(new_pieces, &spt));
  *detokenized = std::move(spt.text());

  return util::OkStatus();
}

util::Status SentencePieceProcessor::Decode(const std::vector<int> &ids,
                                            std::string *detokenized) const {
  CHECK_OR_RETURN_STATUS_STL(detokenized);

  auto new_ids = expand_from_ids(ids);
  SentencePieceText spt;
  RETURN_IF_ERROR(Decode(new_ids, &spt));
  *detokenized = std::move(spt.text());

  return util::OkStatus();
}

util::Status SentencePieceProcessor::NBestEncode(
    absl::string_view input, int nbest_size,
    std::vector<std::vector<std::string>> *pieces) const {
  CHECK_OR_RETURN_STATUS_STL(pieces);

  NBestSentencePieceText spt;
  RETURN_IF_ERROR(NBestEncode(input, nbest_size, &spt));
  for (const auto &nbest : spt.nbests()) {
    std::vector<std::string> result;
    for (const auto &sp : nbest.pieces()) {
      result.emplace_back(sp.piece());
    }
    pieces->emplace_back(result);
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::NBestEncode(
    absl::string_view input, int nbest_size,
    std::vector<std::vector<int>> *ids) const {
  CHECK_OR_RETURN_STATUS_STL(ids);

  NBestSentencePieceText spt;
  RETURN_IF_ERROR(NBestEncode(input, nbest_size, &spt));
  for (const auto &nbest : spt.nbests()) {
    std::vector<int> result;
    for (const auto &sp : nbest.pieces()) {
      result.emplace_back(sp.id());
    }
    ids->emplace_back(result);
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::SampleEncode(
    absl::string_view input, int nbest_size, float alpha,
    std::vector<std::string> *pieces) const {
  CHECK_OR_RETURN_STATUS_STL(pieces);

  SentencePieceText spt;
  RETURN_IF_ERROR(SampleEncode(input, nbest_size, alpha, &spt));
  for (const auto &sp : spt.pieces()) {
    pieces->emplace_back(sp.piece());
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::SampleEncode(absl::string_view input,
                                                  int nbest_size, float alpha,
                                                  std::vector<int> *ids) const {
  CHECK_OR_RETURN_STATUS_STL(ids);

  SentencePieceText spt;
  RETURN_IF_ERROR(SampleEncode(input, nbest_size, alpha, &spt));
  for (const auto &sp : spt.pieces()) {
    ids->emplace_back(sp.id());
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::PopulateSentencePieceText(
    absl::string_view input, absl::string_view normalized,
    const std::vector<size_t> &norm_to_orig, const EncodeResult &result,
    SentencePieceText *spt) const {
  size_t consumed = 0;
  bool is_prev_unk = false;
  for (const auto &p : result) {
    const absl::string_view w = p.first;  // piece
    const int id = p.second;              // id

    CHECK_OR_RETURN(!w.empty()) << "Empty piece is not allowed.";

    const bool is_unk = IsUnknown(id);

    if (IsControl(id)) {
      // Control symbol has no corresponding source surface, so begin == end.
      auto *sp = spt->add_pieces();
      sp->set_piece(w.data(), w.size());
      sp->set_id(id);
      sp->set_begin(norm_to_orig[consumed]);
      sp->set_end(norm_to_orig[consumed]);
    } else {
      const size_t begin = consumed;
      const size_t end = consumed + w.size();
      CHECK_LT_OR_RETURN(begin, norm_to_orig.size());
      CHECK_LT_OR_RETURN(end, norm_to_orig.size());
      const size_t orig_begin = norm_to_orig[begin];
      const size_t orig_end = norm_to_orig[end];
      CHECK_LE_OR_RETURN(orig_begin, input.size());
      CHECK_LE_OR_RETURN(orig_end, input.size());
      CHECK_LE_OR_RETURN(orig_begin, orig_end);
      const auto surface =
          absl::ClippedSubstr(input, orig_begin, orig_end - orig_begin);

      if (is_unk && model_->ByteFallbackEnabled()) {
        // Decomposes an unknown piece into UTF-8 bytes
        for (int i = 0; i < w.size(); ++i) {
          // Create a byte piece
          const char b = w[i];
          auto *sp = spt->add_pieces();
          const auto piece = ByteToPiece(b);
          auto sp_id = model_->PieceToId(piece);
          sp->set_piece(piece.data(), piece.size());
          sp->set_id(sp_id);

          // The last byte piece holds the surface of the original unknown
          // character. The other byte pieces have no surface.
          if (i == w.size() - 1) {
            sp->set_surface(surface.data(), surface.size());
            sp->set_begin(orig_begin);
            sp->set_end(orig_end);
          } else {
            // begin == end
            sp->set_begin(orig_begin);
            sp->set_end(orig_begin);
          }
        }
      } else {
        // Merges continuous run of unknown pieces so that decoder
        // can copy or generate unknown tokens easily.
        // Note that merged tokens are still unknown,
        // since known pieces never consist of unknown characters.
        if (is_prev_unk && is_unk) {
          auto *sp = spt->mutable_pieces(spt->pieces_size() - 1);
          sp->set_piece(sp->piece() + std::string(w));
          sp->set_surface(sp->surface() + std::string(surface));
          sp->set_end(orig_end);
        } else {
          auto *sp = spt->add_pieces();
          sp->set_piece(w.data(), w.size());
          sp->set_id(id);
          sp->set_surface(surface.data(), surface.size());
          sp->set_begin(orig_begin);
          sp->set_end(orig_end);
        }
      }
      consumed += w.size();
    }
    is_prev_unk = is_unk;
  }

  CHECK_EQ_OR_RETURN(consumed, normalized.size())
      << "all normalized characters are not consumed.";

  RETURN_IF_ERROR(ApplyExtraOptions(encode_extra_options_, spt));

  spt->set_text(input.data(), input.size());

  return util::OkStatus();
}  // namespace sentencepiece

util::Status SentencePieceProcessor::Encode(absl::string_view input,
                                            SentencePieceText *spt) const {
  CHECK_OR_RETURN_STATUS_PROTO(spt);

  std::string normalized;
  std::vector<size_t> norm_to_orig;
  RETURN_IF_ERROR(normalizer_->Normalize(input, &normalized, &norm_to_orig));

  const auto result = model_->Encode(normalized);
  RETURN_IF_ERROR(
      PopulateSentencePieceText(input, normalized, norm_to_orig, result, spt));

  return util::OkStatus();
}

util::Status SentencePieceProcessor::NBestEncode(
    absl::string_view input, int nbest_size,
    NBestSentencePieceText *nbest_spt) const {
  CHECK_OR_RETURN_STATUS_PROTO(nbest_spt);

  std::string normalized;
  std::vector<size_t> norm_to_orig;
  RETURN_IF_ERROR(normalizer_->Normalize(input, &normalized, &norm_to_orig));

  CHECK_OR_RETURN(model_->IsNBestEncodeAvailable())
      << "NBestEncode is not available for the current model.";

  const auto nbests = model_->NBestEncode(normalized, nbest_size);
  CHECK_OR_RETURN(!nbests.empty()) << "NBestEncode returns empty result.";

  for (const auto &result : nbests) {
    auto *spt = nbest_spt->add_nbests();
    spt->set_score(result.second);
    RETURN_IF_ERROR(PopulateSentencePieceText(input, normalized, norm_to_orig,
                                              result.first, spt));
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::SampleEncode(
    absl::string_view input, int nbest_size, float alpha,
    SentencePieceText *spt) const {
  CHECK_OR_RETURN_STATUS_PROTO(spt);

  CHECK_LE_OR_RETURN(nbest_size, 512) << "nbest_size must be nbest_size <= 512";

  std::string normalized;
  std::vector<size_t> norm_to_orig;
  RETURN_IF_ERROR(normalizer_->Normalize(input, &normalized, &norm_to_orig));

  if (!model_->IsNBestEncodeAvailable() || nbest_size < 0) {
    CHECK_OR_RETURN(model_->IsSampleEncodeAvailable())
        << "SampleEncode is not available for the current model.";
    const auto result = model_->SampleEncode(normalized, alpha);
    RETURN_IF_ERROR(PopulateSentencePieceText(input, normalized, norm_to_orig,
                                              result, spt));
  } else if (nbest_size == 1 || nbest_size == 0) {
    const auto result = model_->Encode(normalized);
    RETURN_IF_ERROR(PopulateSentencePieceText(input, normalized, norm_to_orig,
                                              result, spt));
  } else if (nbest_size > 1) {
    const auto nbests = model_->NBestEncode(normalized, nbest_size);
    CHECK_OR_RETURN(!nbests.empty()) << "NBestEncode returns empty result.";

    std::vector<float> probs(nbests.size(), 0.0);
    for (size_t i = 0; i < nbests.size(); ++i) {
      probs[i] = std::exp(alpha * nbests[i].second);
    }

    auto *mt = random::GetRandomGenerator();
    std::discrete_distribution<int> dist(probs.begin(), probs.end());
    RETURN_IF_ERROR(PopulateSentencePieceText(input, normalized, norm_to_orig,
                                              nbests[dist(*mt)].first, spt));
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::Decode(
    const std::vector<std::string> &pieces, SentencePieceText *spt) const {
  CHECK_OR_RETURN_STATUS_PROTO(spt);

  const char *unk_surface = kDefaultUnknownSymbol;
  if (model_proto_ && model_proto_->trainer_spec().has_unk_surface())
    unk_surface = model_proto_->trainer_spec().unk_surface().c_str();

  auto DecodeSentencePiece = [&](absl::string_view piece, int id,
                                 bool is_bos_ws, bool is_eos_ws) -> std::string {
    if (IsControl(id)) {  // <s>, </s>
      return "";          // invisible symbol.
    } else if (IsUnknown(id)) {
      if (IdToPiece(id) == piece) {  // <unk>
        return unk_surface;
      } else {  // return piece when piece is not <unk>.
        return std::string(piece);
      }
    }

    if(!model_proto_ || !model_proto_->has_trainer_spec()
       || !model_proto_->trainer_spec().treat_whitespace_as_suffix()) {
      if(is_bos_ws &&
          (!model_proto_ ||
           (model_proto_ &&
            (model_proto_->normalizer_spec().add_dummy_prefix() ||
             model_proto_->normalizer_spec().remove_extra_whitespaces())))) {
        // Consume if the current position is bos and
        // piece starts with kSpaceSymbol.
        absl::ConsumePrefix(&piece, kSpaceSymbol);
      }
    } else {
        if(is_eos_ws &&
            (!model_proto_ ||
             (model_proto_ &&
              (model_proto_->normalizer_spec().add_dummy_prefix() ||
               model_proto_->normalizer_spec().remove_extra_whitespaces())))) {
          // Consume if the current position is eos and
          // piece ends with kSpaceSymbol.
          if(absl::EndsWith(piece, kSpaceSymbol))
            piece.remove_suffix(3);
        }
    }

    return absl::StrReplaceAll(piece, {{kSpaceSymbol, " "}});
  };

  for (const std::string &w : pieces) {
    auto *sp = spt->add_pieces();
    sp->set_piece(w);
    sp->set_id(PieceToId(w));
  }

  RETURN_IF_ERROR(ApplyExtraOptions(decode_extra_options_, spt));

  std::string *text = spt->mutable_text();
  auto SetSurface = [&](int index, const std::string &surface) {
    auto *sp = spt->mutable_pieces(index);
    sp->set_surface(surface);
    sp->set_begin(text->size());
    sp->set_end(text->size() + surface.size());
    *text += surface;
  };
  auto ProcessBytePieces = [&](int begin, int end) -> util::Status {
    if (begin < end) {
      // Constructs byte sequence.
      std::string bytes;
      for (int i = begin; i < end; ++i) {
        const auto &sp = spt->pieces(i);
        const int byte = PieceToByte(sp.piece());
        CHECK_LE_OR_RETURN(0, byte);
        bytes.append(1, byte);
      }
      // Decodes byte sequence as UTF-8 and encodes the result into UTF-8 bytes
      // again.
      int i = begin;
      for (const char32 uc :
           string_util::UTF8ToUnicodeText(absl::string_view(bytes))) {
        if (uc == kUnicodeError) {
          // Invalid UTF-8 bytes are mapped to REPLACEMENT CHARACTER (U+FFFD).
          SetSurface(i++, string_util::UnicodeCharToUTF8(kUnicodeError));
        } else {
          const std::string utf8 = string_util::UnicodeCharToUTF8(uc);
          for (int j = 0; j < utf8.size(); j++) {
            // The last byte piece holds the surface of the original unknown
            // character. The other byte pieces hold an empty string as
            // surface.
            if (j == utf8.size() - 1) {
              SetSurface(i++, utf8);
            } else {
              SetSurface(i++, "");
            }
          }
        }
      }
      CHECK_EQ_OR_RETURN(i, end);
    }
    return util::OkStatus();
  };
  int byte_start = 0;
  for (int i = 0; i < spt->pieces_size(); ++i) {
    const auto &sp = spt->pieces(i);
    if (!IsByte(sp.id())) {
      RETURN_IF_ERROR(ProcessBytePieces(byte_start, i));
      byte_start = i + 1;
      bool is_eos_space = i == spt->pieces_size() - 1;
      SetSurface(i, DecodeSentencePiece(sp.piece(), sp.id(), text->empty(), is_eos_space));
    }
  }
  RETURN_IF_ERROR(ProcessBytePieces(byte_start, spt->pieces_size()));

  // If there is a denormalizer, we need to remap the surface strings on
  // the individual pieces based on the norm_to_orig mapping from the
  // denormalizer. Otherwise, if the number of characters differ across the
  // denormalized and normalized form, the surface strings would be the 
  // pre-denormalized surface strings, rather than post-denormalized. This
  // is particularly a problem with case encoding.
  if (denormalizer_) {
    std::string normalized;
    std::vector<size_t> norm_to_orig;
    denormalizer_->Normalize(*text, &normalized, &norm_to_orig);
    *text = normalized;

    // Since this is denormalization, we really want orig_to_norm mapping
    // here instead. Constructing that
    std::map<int, int> orig_to_norm;
    for(int i = 0; i < norm_to_orig.size(); i++) {
      if (orig_to_norm.find(norm_to_orig[i]) == orig_to_norm.end()) {
        orig_to_norm[norm_to_orig[i]] = i;
      }
    }

    int normalized_piece_surface_index = 0;
    int text_piece_surface_index = 0;
    int last_consumed_byte = -1;
    for(int i = 0; i < spt->pieces_size(); i++) {
      auto *spiece = spt->mutable_pieces(i);
      auto curr_surface = spiece->surface();

      // Determine the new surface string based on orig_to_norm indices
      std::string new_surface;
      for(int j = text_piece_surface_index; j < text_piece_surface_index + curr_surface.size();
          j++) {
        auto norm_index = orig_to_norm.find(j + 1);
        if(norm_index != orig_to_norm.end()) {
          for(int k = last_consumed_byte + 1; k <=  norm_index->second - 1; k++)
            new_surface.push_back(normalized[k]);
          last_consumed_byte = norm_index->second - 1;
        }
      }

      text_piece_surface_index += curr_surface.size();

      // Reset the piece information with updated surface string
      spiece->set_surface(new_surface);
      spiece->set_begin(normalized_piece_surface_index);
      normalized_piece_surface_index += new_surface.size();
      spiece->set_end(normalized_piece_surface_index);
    }
  }

  return util::OkStatus();
}

util::Status SentencePieceProcessor::Decode(const std::vector<int> &ids,
                                            SentencePieceText *spt) const {
  std::vector<std::string> pieces;
  pieces.reserve(ids.size());
  for (const int id : ids) {
    pieces.emplace_back(IdToPiece(id));
  }
  return Decode(pieces, spt);
}

std::string SentencePieceProcessor::EncodeAsSerializedProto(
    absl::string_view input) const {
  SentencePieceText spt;
  if (!Encode(input, &spt).ok()) return "";
  return spt.SerializeAsString();
}

std::string SentencePieceProcessor::SampleEncodeAsSerializedProto(
    absl::string_view input, int nbest_size, float alpha) const {
  SentencePieceText spt;
  if (!SampleEncode(input, nbest_size, alpha, &spt).ok()) return "";
  return spt.SerializeAsString();
}

std::string SentencePieceProcessor::NBestEncodeAsSerializedProto(
    absl::string_view input, int nbest_size) const {
  NBestSentencePieceText spt;
  if (!NBestEncode(input, nbest_size, &spt).ok()) return "";
  return spt.SerializeAsString();
}

std::string SentencePieceProcessor::DecodePiecesAsSerializedProto(
    const std::vector<std::string> &pieces) const {
  SentencePieceText spt;
  if (!Decode(pieces, &spt).ok()) return "";
  return spt.SerializeAsString();
}

std::string SentencePieceProcessor::DecodeIdsAsSerializedProto(
    const std::vector<int> &ids) const {
  SentencePieceText spt;
  if (!Decode(ids, &spt).ok()) return "";
  return spt.SerializeAsString();
}

#define CHECK_STATUS_OR_RETURN_DEFAULT(value)                                \
  if (!status().ok()) {                                                      \
    LOG(ERROR) << status().message() << "\nReturns default value " << value; \
    return value;                                                            \
  }

int SentencePieceProcessor::GetPieceSize() const {
  CHECK_STATUS_OR_RETURN_DEFAULT(0);
  return model_->GetPieceSize();
}

int SentencePieceProcessor::PieceToId(absl::string_view piece) const {
  CHECK_STATUS_OR_RETURN_DEFAULT(0);
  return model_->PieceToId(piece);
}

const std::string &SentencePieceProcessor::IdToPiece(int id) const {
  static const std::string *kEmptyString = new std::string;
  CHECK_STATUS_OR_RETURN_DEFAULT(*kEmptyString);
  return model_->IdToPiece(id);
}

float SentencePieceProcessor::GetScore(int id) const {
  CHECK_STATUS_OR_RETURN_DEFAULT(0.0);
  return model_->GetScore(id);
}

bool SentencePieceProcessor::IsControl(int id) const {
  CHECK_STATUS_OR_RETURN_DEFAULT(0);
  return model_->IsControl(id);
}

bool SentencePieceProcessor::IsUnknown(int id) const {
  CHECK_STATUS_OR_RETURN_DEFAULT(0);
  return model_->IsUnknown(id);
}

bool SentencePieceProcessor::IsUnused(int id) const {
  CHECK_STATUS_OR_RETURN_DEFAULT(false);
  return model_->IsUnused(id);
}

bool SentencePieceProcessor::IsByte(int id) const {
  CHECK_STATUS_OR_RETURN_DEFAULT(false);
  return model_->IsByte(id);
}

int SentencePieceProcessor::unk_id() const {
  const int id = PieceToId(absl::string_view(model_->unk_piece().data()));
  if (IsUnknown(id)) return id;
  return -1;
}

int SentencePieceProcessor::bos_id() const {
  const int id = PieceToId(absl::string_view(model_->bos_piece().data()));
  if (IsControl(id)) return id;
  return -1;
}

int SentencePieceProcessor::eos_id() const {
  const int id = PieceToId(absl::string_view(model_->eos_piece().data()));
  if (IsControl(id)) return id;
  return -1;
}

int SentencePieceProcessor::pad_id() const {
  const int id = PieceToId(absl::string_view(model_->pad_piece().data()));
  if (IsControl(id)) return id;
  return -1;
}

// static
util::Status SentencePieceProcessor::ApplyExtraOptions(
    const std::vector<ExtraOption> &extra_options,
    SentencePieceText *spt) const {
  for (const auto &extra_option : extra_options) {
    switch (extra_option) {
      case REVERSE:
        std::reverse(spt->mutable_pieces()->begin(),
                     spt->mutable_pieces()->end());
        break;
      case EOS: {
        auto *piece = spt->add_pieces();
        piece->set_id(PieceToId(absl::string_view(model_->eos_piece().data())));
        piece->set_piece(model_->eos_piece().data(),
                         model_->eos_piece().size());
      } break;
      case BOS: {
        auto *array = spt->mutable_pieces();
        array->Add();
        for (int i = array->size() - 1; i > 0; --i) {
          array->SwapElements(i - 1, i);
        }
        auto *piece = array->Mutable(0);
        piece->set_id(PieceToId(absl::string_view(model_->bos_piece().data())));
        piece->set_piece(model_->bos_piece().data(),
                         model_->bos_piece().size());
      } break;
      default:
        return util::InternalError("unknown extra_option type.");
    }
  }

  return util::OkStatus();
}

// static
util::Status SentencePieceProcessor::ParseExtraOptions(
    absl::string_view _extra_option,
    std::vector<SentencePieceProcessor::ExtraOption> *extra_options) const {
  absl::string_view extra_option(_extra_option.data(), _extra_option.size());

  extra_options->clear();
  if (extra_option.empty()) return util::OkStatus();

  RETURN_IF_ERROR(status());

  static std::map<absl::string_view, SentencePieceProcessor::ExtraOption>
      extra_option_map = {{"bos", SentencePieceProcessor::BOS},
                          {"eos", SentencePieceProcessor::EOS},
                          {"reverse", SentencePieceProcessor::REVERSE}};
  for (const auto &s : absl::StrSplit(extra_option, ":")) {
    const auto it = extra_option_map.find(s);
    CHECK_OR_RETURN(it != extra_option_map.end())
        << "option \"" << s << "\" is not available.";
    extra_options->push_back(it->second);

    if (it->second == SentencePieceProcessor::BOS) {
      CHECK_OR_RETURN(
          !IsUnknown(PieceToId(absl::string_view(model_->bos_piece().data()))))
          << "id for `" << model_->bos_piece() << "` is not defined.";
    }
    if (it->second == SentencePieceProcessor::EOS) {
      CHECK_OR_RETURN(
          !IsUnknown(PieceToId(absl::string_view(model_->eos_piece().data()))))
          << "id for `" << model_->eos_piece() << "` is not defined.";
    }
  }
  return util::OkStatus();
}

void SentencePieceProcessor::SetModel(std::unique_ptr<ModelInterface> &&model) {
  model_ = std::move(model);
}

void SentencePieceProcessor::SetNormalizer(
    std::unique_ptr<normalizer::Normalizer> &&normalizer) {
  normalizer_ = std::move(normalizer);
}

const ModelProto &SentencePieceProcessor::model_proto() const {
  return *model_proto_;
}

std::string SentencePieceProcessor::serialized_model_proto() const {
  return model_proto_ ? model_proto_->SerializeAsString() : "";
}

namespace io {

util::Status LoadModelProto(absl::string_view filename,
                            ModelProto *model_proto) {
  if (filename.empty()) {
    return util::NotFoundError("model file path should not be empty.");
  }

  auto input = filesystem::NewReadableFile(filename, true);
  RETURN_IF_ERROR(input->status());
  std::string serialized;
  CHECK_OR_RETURN(input->ReadAll(&serialized));
  CHECK_OR_RETURN(
      model_proto->ParseFromArray(serialized.data(), serialized.size()));

  return util::OkStatus();
}

util::Status SaveModelProto(absl::string_view filename,
                            const ModelProto &model_proto) {
  if (filename.empty()) {
    return util::NotFoundError("model file path should not be empty.");
  }
  auto output = filesystem::NewWritableFile(filename, true);
  RETURN_IF_ERROR(output->status());
  CHECK_OR_RETURN(output->Write(model_proto.SerializeAsString()));

  return util::OkStatus();
}
}  // namespace io
}  // namespace sentencepiece
