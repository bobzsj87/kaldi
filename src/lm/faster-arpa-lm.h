// lm/const-arpa-lm.h

// Copyright 2018  Zhehuai Chen

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#ifndef KALDI_LM_FASTER_ARPA_LM_H_
#define KALDI_LM_FASTER_ARPA_LM_H_

#include <string>
#include <vector>
#include <math.h>

#include "base/kaldi-common.h"
#include "fstext/deterministic-fst.h"
#include "lm/arpa-file-parser.h"
#include "util/common-utils.h"

namespace kaldi {

uint64  RandInt64() {
  uint64_t random =
  (((uint64_t) rand() <<  0) & 0x000000000000FFFFull) ^ 
  (((uint64_t) rand() << 16) & 0x00000000FFFF0000ull) ^ 
  (((uint64_t) rand() << 32) & 0x0000FFFF00000000ull) ^
  (((uint64_t) rand() << 48) & 0xFFFF000000000000ull);
  return random;
}
#define MAX_NGRAM 5+1
#define RAND_TYPE int64
#define HASH_REDUNDANT 0.5
class FasterArpaLm {
 public:
  typedef fst::StdArc::StateId StateId;

  // LmState in FasterArpaLm: the basic storage unit
  class LmState {
   public:
    LmState(): logprob_(0), h_value(0), word_ids_(NULL), next(NULL) { }
    LmState(float logprob, float backoff_logprob): 
      logprob_(logprob), backoff_logprob_(backoff_logprob), h_value(0), word_ids_(NULL),
    next(NULL) { }
    void Allocate(const NGram* ngram, float lm_scale=1) {
      logprob_ = ngram->logprob*lm_scale;
      backoff_logprob_ = ngram->backoff*lm_scale;
      /*
      std::vector<int32> &word_ids = ngram->words;
      int32 ngram_order = word_ids.size();
      int32 sz= sizeof(int32)*(ngram_order);
      */
    }
    void SaveWordIds(const int32 *word_ids, const int32 ngram_order) {
      word_ids_ = (int32 *)malloc(sizeof(int32)*ngram_order);
      for (int i=0; i<ngram_order; i++) word_ids_[i] = word_ids[i];
      ngram_order_ = ngram_order;
    }
    bool IsExist() const { return logprob_!=0; }
    ~LmState() { if (word_ids_) free(word_ids_); }

    // for current query
    float logprob_;
    // for next query; can be optional
    float backoff_logprob_;
    RAND_TYPE h_value;
    int32 *word_ids_;
    int32 ngram_order_;
    LmState* next; // for colid
    int32 lm_state_reco_;
  };

  // Class to build FasterArpaLm from Arpa format language model. It relies on the
  // auxiliary class LmState above.
  class FasterArpaLmBuilder : public ArpaFileParser {
   public:
    FasterArpaLmBuilder(ArpaParseOptions &options, FasterArpaLm *lm, 
      float lm_scale = 1): ArpaFileParser(options, NULL),
    lm_(lm), lm_scale_(lm_scale) { }
    ~FasterArpaLmBuilder() { }

   protected:
    // ArpaFileParser overrides.
    virtual void HeaderAvailable() {
      lm_->Allocate(NgramCounts(), 
          Options().bos_symbol, Options().eos_symbol, Options().unk_symbol);
    }
    virtual void ConsumeNGram(const NGram& ngram) {
      LmState lm_state(ngram.logprob * lm_scale_, ngram.backoff * lm_scale_);
      lm_->SaveHashedState(ngram.words, lm_state, true);
    }

    virtual void ReadComplete()  { 
      lm_->SaveRecoState();
    }

   private:
    FasterArpaLm *lm_;
    float lm_scale_;
  };

  FasterArpaLm(ArpaParseOptions &options, const std::string& arpa_rxfilename,
    int32 symbol_size, float lm_scale=1): symbol_size_(symbol_size), options_(options) {
    assert(symbol_size_);
    is_built_ = false;
    ngram_order_ = 0;
    num_words_ = 0;
    lm_states_size_ = 0;
    randint_per_word_gram_ = NULL;
    max_collision_ = 0;

    BuildFasterArpaLm(arpa_rxfilename, lm_scale);
    assert(ngrams_num_ >= ngrams_saved_num_);
    if (ngrams_num_ != ngrams_saved_num_) {
      KALDI_WARN << "num mismatch in arpa header: "<<ngrams_num_<<" "<<ngrams_saved_num_;
    }
    KALDI_VLOG(2) << max_collision_;
  }

  ~FasterArpaLm() {
    if (is_built_) Free();
  }

  int32 BosSymbol() const { return bos_symbol_; }
  int32 EosSymbol() const { return eos_symbol_; }
  int32 UnkSymbol() const { return unk_symbol_; }
  int32 NgramOrder() const { return ngram_order_; }

  inline int32 GetHashedIdx(const int32* word_ids, 
      int query_ngram_order, RAND_TYPE *h_value=NULL) const {
    assert(query_ngram_order > 0 && query_ngram_order <= ngram_order_);
    int32 ngram_order = query_ngram_order;
    RAND_TYPE hashed_idx;
    if (ngram_order == 1) {
      hashed_idx = word_ids[ngram_order-1];
    } else {
      hashed_idx=randint_per_word_gram_[0][word_ids[0]];
      for (int i=1; i<ngram_order; i++) {
        int word_id=word_ids[i];
        hashed_idx ^= randint_per_word_gram_[i][word_id];
      }
      if (h_value) *h_value = hashed_idx; // to check colid, h_value should be precise
      int i = ngram_order-1;
      hashed_idx &= 
          (ngrams_hashed_size_[i]-ngrams_hashed_size_[i-1] - 1);
      hashed_idx += ngrams_hashed_size_[i-1];
    }
    return hashed_idx;
  }
  inline void InsertHash(int64 hashed_idx, int64 ngrams_saved_num_) {
    assert(hashed_idx < ngrams_map_.size());
    if (ngrams_map_.at(hashed_idx)) {
      LmState *lm_state = ngrams_map_[hashed_idx];
      int32 cnt=0;
      while (lm_state->next) {
        lm_state = lm_state->next;
        cnt++;
      }
      lm_state->next = &ngrams_[ngrams_saved_num_];
      max_collision_=std::max(cnt,max_collision_);
    } else {
      ngrams_map_[hashed_idx] = &ngrams_[ngrams_saved_num_];
      assert(ngrams_saved_num_ < ngrams_num_);
    }
  }
  inline void SaveHashedState(const int32* word_ids, 
      int query_ngram_order, LmState &lm_state_pattern) {
    RAND_TYPE h_value=0;
    int64 hashed_idx = GetHashedIdx(word_ids, query_ngram_order, &h_value);
    lm_state_pattern.h_value = h_value;
    int32 ngram_order = query_ngram_order;
    if (ngram_order == 1) {
      ngrams_[hashed_idx] = lm_state_pattern;
      ngrams_[hashed_idx].SaveWordIds(word_ids, ngram_order);
    } else {
      ngrams_[ngrams_saved_num_] = lm_state_pattern;
      ngrams_[ngrams_saved_num_].SaveWordIds(word_ids, ngram_order);
      InsertHash(hashed_idx, ngrams_saved_num_++);
    }
  }
  inline void SaveHashedState(const std::vector<int32> &word_ids, LmState &lm_state_pattern,
       bool reverse = false, int query_ngram_order = 0)  {
    int32 ngram_order = query_ngram_order==0? word_ids.size(): query_ngram_order;
    int32 word_ids_arr[MAX_NGRAM];
    if (reverse)
      for (int i=0; i<ngram_order;i++) word_ids_arr[ngram_order - i - 1]=word_ids[i];
    else
      for (int i=0; i<ngram_order;i++) word_ids_arr[i]=word_ids[i];

    return SaveHashedState(word_ids_arr, ngram_order, lm_state_pattern);
  }

  inline void SaveRecoState() {
    for (int i=0; i<ngrams_num_; i++) {
      int32 *word_ids = ngrams_[i].word_ids_;
      int32 ngram_order = ngrams_[i].ngram_order_;
      StateId lm_state_idx;
      if (ngram_order > ngram_order_-1) {
        ngram_order--;
        while(!GetHashedState(word_ids, ngram_order, &lm_state_idx)) ngram_order--;
        assert(ngram_order>0);
      } else lm_state_idx = i; 
      ngrams_[i].lm_state_reco_ = lm_state_idx;
    }
  }

  inline const LmState* GetHashedState(const int32* word_ids, 
      int query_ngram_order, StateId *lm_state_idx=NULL) const {
    RAND_TYPE h_value;
    LmState *ret_lm_state = NULL;
    int64 hashed_idx = GetHashedIdx(word_ids, query_ngram_order, &h_value);
    int32 ngram_order = query_ngram_order;
    if (ngram_order == 1) {
      ret_lm_state = &ngrams_[hashed_idx];
    } else {
      assert(hashed_idx < ngrams_map_.size());
      LmState *lm_state = ngrams_map_[hashed_idx];
      while (lm_state) {
        if (lm_state->h_value == h_value) {
          ret_lm_state = lm_state;
          break;
        }
        lm_state = lm_state->next;
      }
    }
    if (ret_lm_state && lm_state_idx) *lm_state_idx = ret_lm_state - ngrams_;
   
    // not found, can be bug or really not found the corresponding ngram 
    return ret_lm_state;
  }
  inline const LmState* GetHashedState(const std::vector<int32> &word_ids, 
       bool reverse = false, int query_ngram_order = 0) const {
    int32 ngram_order = query_ngram_order==0? word_ids.size(): query_ngram_order;
    int32 word_ids_arr[MAX_NGRAM];
    if (reverse)
      for (int i=0; i<ngram_order;i++) word_ids_arr[ngram_order - i - 1]=word_ids[i];
    else
      for (int i=0; i<ngram_order;i++) word_ids_arr[i]=word_ids[i];
    return GetHashedState(word_ids_arr, ngram_order);
  }

  // if exist, get logprob_, else get backoff_logprob_
  // memcpy(n_wids+1, wids, len(wids)); n_wids[0] = cur_wrd;
  inline void GetWordIdsByLmStateIdx(int32 **word_ids, 
      int32 *word_ngram_order, int64 lm_state_idx) const {
    *word_ids = ngrams_[lm_state_idx].word_ids_;
    *word_ngram_order = ngrams_[lm_state_idx].ngram_order_;
  }

  inline float GetNgramLogprob(const int32 *word_ids, 
      const int32 word_ngram_order, 
      StateId *lm_state_idx) const {
    float prob;
    int32 ngram_order = word_ngram_order;
    assert(ngram_order > 0);
    if (ngram_order > ngram_order_) {
      //while (wseq.size() >= lm_.NgramOrder()) {
      // History state has at most lm_.NgramOrder() -1 words in the state.
      // wseq.erase(wseq.begin(), wseq.begin() + 1);
      //}
      // we don't need to do above things as we do in reverse fashion:
      //  memcpy(n_wids+1, wids, len(wids)); n_wids[0] = cur_wrd;
      ngram_order = ngram_order_;
    }

    const LmState *lm_state = GetHashedState(word_ids, ngram_order, lm_state_idx);
    if (lm_state) { //found out
      assert(lm_state->IsExist());
      //assert(ngram_order==1 || GetHashedState(word_ids, ngram_order-1)->IsExist());
      prob = lm_state->logprob_;
     
/* 
      for (int i=0; i<ngram_order; i++) {
        std::cout<<word_ids[i]<<" ";
      }
      std::cout<<ngram_order<<" "<<prob<<"\n";
  */   
#define IMPROVE_RECOMBINE
#ifdef IMPROVE_RECOMBINE
#if 1
      *lm_state_idx = lm_state->lm_state_reco_;
#else
      if (ngram_order > ngram_order_-1) {
        ngram_order = ngram_order_-1;
        // below code is to make sure the LmState exist, so un-exist states can be recombined to a same state; 
        // however, it wastes some hashing if we never use the nextstate
        while(!GetHashedState(word_ids, ngram_order, lm_state_idx)) ngram_order--;
        assert(ngram_order>0);
      }
#endif
#endif
    } else {
      assert(ngram_order > 1); // thus we can do backoff
      const LmState *lm_state_bo = GetHashedState(word_ids + 1, ngram_order-1); 

      //assert(lm_state_bo && lm_state_bo->IsExist()); // TODO: assert will fail because some place has false-exist? 84746 4447 8537 without 4447 8537 in LM

      prob = lm_state_bo? lm_state_bo->backoff_logprob_:0;
      prob += GetNgramLogprob(word_ids, ngram_order - 1, lm_state_idx);
    }
    return prob;
  }

  bool BuildFasterArpaLm(const std::string& arpa_rxfilename, float lm_scale) {
    FasterArpaLmBuilder lm_builder(options_, this, lm_scale);
    KALDI_VLOG(1) << "Reading " << arpa_rxfilename;
    Input ki(arpa_rxfilename);
    lm_builder.Read(ki.Stream());
    return true;
  }

 private:
  void Allocate(const std::vector<int32>& ngram_count, 
                int32 bos_symbol, int32 eos_symbol, 
                int32 unk_symbol) {
    bos_symbol_ = bos_symbol;
    eos_symbol_ = eos_symbol;
    unk_symbol_ = unk_symbol;
    ngram_order_ = ngram_count.size();
    srand(0);
    randint_per_word_gram_ = (RAND_TYPE **)malloc(ngram_order_ * sizeof(void*));
    ngrams_hashed_size_ = (int64*)malloc(ngram_order_ * sizeof(int64));
    int64 acc=0;
    int64 acc_hashed=0;
    for (int i=0; i< ngram_order_; i++) {
      if (i == 0) ngrams_hashed_size_[i] = symbol_size_; // uni-gram
      else {
        ngrams_hashed_size_[i] = ((int64)1<<(int64)ceil(log(ngram_count[i]) / 
                                 M_LN2 + HASH_REDUNDANT));
      }
      assert(ngram_count[i] >= 0);
      KALDI_VLOG(2) << "ngram: "<< i+1 <<" hashed_size/size = "<< 
        1.0 * ngrams_hashed_size_[i] / ngram_count[i]<<" "<<ngram_count[i];
      assert(ngrams_hashed_size_[i] >= 0);
      randint_per_word_gram_[i] = (RAND_TYPE* )malloc(symbol_size_ * sizeof(RAND_TYPE)) ;
      for (int j=0; j<symbol_size_; j++) {
        randint_per_word_gram_[i][j] = RandInt64(); 
      }
      acc+= i==0? ngrams_hashed_size_[i]:ngram_count[i];
      acc_hashed+= ngrams_hashed_size_[i];
      if (i==0) ngrams_hashed_size_[i]=0;
      else ngrams_hashed_size_[i]+=ngrams_hashed_size_[i-1];
    }
    hash_size_except_uni_ = acc_hashed - symbol_size_;
    assert(ngrams_hashed_size_[ngram_order_-1]==hash_size_except_uni_);
    KALDI_VLOG(2) << " hashed_size/size = "<< 
        1.0 * (hash_size_except_uni_+symbol_size_) / acc <<" "<<acc;
    
    ngrams_ = (LmState* )calloc(sizeof(LmState), acc); //use default constructo
    ngrams_num_ = acc;
    ngrams_saved_num_ = symbol_size_; // assume uni-gram is allocated
    assert(hash_size_except_uni_ >= 0);
    ngrams_map_.resize(hash_size_except_uni_, NULL);
    is_built_ = true;
  }
  void Free() {
    for (int i=0; i< ngram_order_; i++) {
      free(randint_per_word_gram_[i]);
    }
    free(randint_per_word_gram_);
    free(ngrams_);
  }

 private:
  // configurations

  // Indicating if FasterArpaLm has been built or not.
  bool is_built_;
  // Integer corresponds to <s>.
  int32 bos_symbol_;
  // Integer corresponds to </s>.
  int32 eos_symbol_;
  // Integer corresponds to unknown-word. -1 if no unknown-word symbol is
  // provided.
  int32 unk_symbol_;  
  // N-gram order of language model. This can be figured out from "/data/"
  // section in Arpa format language model.
  int32 ngram_order_;
  int32 symbol_size_;
  // Index of largest word-id plus one. It defines the end of <unigram_states_>
  // array.
  int32 num_words_;
  // Size of the <lm_states_> array, which will be needed by I/O.
  int64 lm_states_size_;
  // Hash table from word sequences to LmStates.
  ArpaParseOptions &options_;

  // data

  // Memory blcok for storing N-gram; ngrams_[ngram_order][hashed_idx]
  LmState* ngrams_;
  int64 ngrams_saved_num_;
  int64 ngrams_num_;

  std::vector<LmState *> ngrams_map_; // hash to ngrams_ index
  // used to obtain hash value; randint_per_word_gram_[ngram_order][word_id]
  RAND_TYPE** randint_per_word_gram_;
  int64* ngrams_hashed_size_; //after init, it's an accumulate value
  int64 hash_size_except_uni_;
  int32 max_collision_;
};


/**
 This class wraps a FasterArpaLm format language model with the interface defined
 in DeterministicOnDemandFst.
 */
class FasterArpaLmDeterministicFst
  : public fst::DeterministicOnDemandFst<fst::StdArc> {
 public:
  typedef fst::StdArc::Weight Weight;
  typedef fst::StdArc::StateId StateId;
  typedef fst::StdArc::Label Label;
  typedef FasterArpaLm::LmState LmState;

  explicit FasterArpaLmDeterministicFst(const FasterArpaLm& lm): 
    start_state_(0), lm_(lm) { 
      // TODO
    // Creates a history state for <s>.
    int32 word_ids = lm_.BosSymbol();
    lm_.GetNgramLogprob(&word_ids, 1, &start_state_);
  }

  // We cannot use "const" because the pure virtual function in the interface is
  // not const.
  virtual StateId Start() { return start_state_; }

  // We cannot use "const" because the pure virtual function in the interface is
  // not const.
  virtual Weight Final(StateId s) {
    // At this point, we should have created the state.
    StateId lm_state_idx;
    float logprob = GetNgramLogprob(s, lm_.EosSymbol(), &lm_state_idx);
    return Weight(-logprob);
  }

  float GetNgramLogprob(const int64 pre_lm_state_idx, int32 ilabel,
      StateId *lm_state_idx) {
    int32 *wseq;
    int32 wseq_order;
    lm_.GetWordIdsByLmStateIdx(&wseq, &wseq_order, pre_lm_state_idx);
    int32 n = wseq_order;
    assert(n>0);
    int32 word_ids[MAX_NGRAM];
    assert(n+1 <= MAX_NGRAM);

    word_ids[0] = ilabel;
    for (int i=0; i<n; i++ ) {
      word_ids[i+1] = wseq[i];
    }

    return lm_.GetNgramLogprob(word_ids, n+1, lm_state_idx);
  }
  virtual bool GetArc(StateId s, Label ilabel, fst::StdArc* oarc) {
    // At this point, we should have created the state.

    StateId lm_state_idx;
    float logprob = GetNgramLogprob(s, ilabel, &lm_state_idx);
    if (logprob == std::numeric_limits<float>::min()) {
      return false;
    }

    // Creates the arc.
    oarc->ilabel = ilabel;
    oarc->olabel = ilabel;
    oarc->nextstate = lm_state_idx;
    oarc->weight = Weight(-logprob);

    return true;
  }

 private:
  StateId start_state_;

  const FasterArpaLm& lm_;
};


}  // namespace kaldi

#endif  // KALDI_LM_CONST_ARPA_LM_H_
