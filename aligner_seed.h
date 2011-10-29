/*
 * Copyright 2011, Ben Langmead <blangmea@jhsph.edu>
 *
 * This file is part of Bowtie 2.
 *
 * Bowtie 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bowtie 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bowtie 2.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ALIGNER_SEED_H_
#define ALIGNER_SEED_H_

#include <iostream>
#include <utility>
#include <limits>
#include "qual.h"
#include "ds.h"
#include "sstring.h"
#include "alphabet.h"
#include "edit.h"
#include "read.h"
// Threading is necessary to synchronize the classes that dump
// intermediate alignment results to files.  Otherwise, all data herein
// is constant and shared, or per-thread.
#include "threading.h"
#include "aligner_cache.h"
#include "scoring.h"
#include "mem_ids.h"
#include "simple_func.h"

/**
 * A constraint to apply to an alignment zone, or to an overall
 * alignment.
 *
 * The constraint can put both caps and ceilings on the number and
 * types of edits allowed.
 */
struct Constraint {
	
	Constraint() { init(); }
	
	/**
	 * Initialize Constraint to be fully permissive.
	 */
	void init() {
		edits = mms = ins = dels = penalty = editsCeil = mmsCeil =
		insCeil = delsCeil = penaltyCeil = std::numeric_limits<int>::max();
		penFunc.reset();
		instantiated = false;
	}
	
	/**
	 * Return true iff penalities and constraints prevent us from
	 * adding any edits.
	 */
	bool mustMatch() {
		assert(instantiated);
		return (mms == 0 && edits == 0) ||
		        penalty == 0 ||
		       (mms == 0 && dels == 0 && ins == 0);
	}
	
	/**
	 * Return true iff a mismatch of the given quality is permitted.
	 */
	bool canMismatch(int q, const Scoring& cm) {
		assert(instantiated);
		return (mms > 0 || edits > 0) &&
		       penalty >= cm.mm(q);
	}

	/**
	 * Return true iff a mismatch of the given quality is permitted.
	 */
	bool canN(int q, const Scoring& cm) {
		assert(instantiated);
		return (mms > 0 || edits > 0) &&
		       penalty >= cm.n(q);
	}
	
	/**
	 * Return true iff a mismatch of *any* quality (even qual=1) is
	 * permitted.
	 */
	bool canMismatch() {
		assert(instantiated);
		return (mms > 0 || edits > 0) && penalty > 0;
	}

	/**
	 * Return true iff a mismatch of *any* quality (even qual=1) is
	 * permitted.
	 */
	bool canN() {
		assert(instantiated);
		return (mms > 0 || edits > 0);
	}
	
	/**
	 * Return true iff a deletion of the given extension (0=open, 1=1st
	 * extension, etc) is permitted.
	 */
	bool canDelete(int ex, const Scoring& cm) {
		assert(instantiated);
		return (dels > 0 && edits > 0) &&
		       penalty >= cm.del(ex);
	}

	/**
	 * Return true iff a deletion of any extension is permitted.
	 */
	bool canDelete() {
		assert(instantiated);
		return (dels > 0 || edits > 0) &&
		       penalty > 0;
	}
	
	/**
	 * Return true iff an insertion of the given extension (0=open,
	 * 1=1st extension, etc) is permitted.
	 */
	bool canInsert(int ex, const Scoring& cm) {
		assert(instantiated);
		return (ins > 0 || edits > 0) &&
		       penalty >= cm.ins(ex);
	}

	/**
	 * Return true iff an insertion of any extension is permitted.
	 */
	bool canInsert() {
		assert(instantiated);
		return (ins > 0 || edits > 0) &&
		       penalty > 0;
	}
	
	/**
	 * Return true iff a gap of any extension is permitted
	 */
	bool canGap() {
		assert(instantiated);
		return ((ins > 0 || dels > 0) || edits > 0) && penalty > 0;
	}
	
	/**
	 * Charge a mismatch of the given quality.
	 */
	void chargeMismatch(int q, const Scoring& cm) {
		assert(instantiated);
		if(mms == 0) { assert_gt(edits, 0); edits--; }
		else mms--;
		penalty -= cm.mm(q);
		assert_geq(mms, 0);
		assert_geq(edits, 0);
		assert_geq(penalty, 0);
	}
	
	/**
	 * Charge an N mismatch of the given quality.
	 */
	void chargeN(int q, const Scoring& cm) {
		assert(instantiated);
		if(mms == 0) { assert_gt(edits, 0); edits--; }
		else mms--;
		penalty -= cm.n(q);
		assert_geq(mms, 0);
		assert_geq(edits, 0);
		assert_geq(penalty, 0);
	}
	
	/**
	 * Charge a deletion of the given extension.
	 */
	void chargeDelete(int ex, const Scoring& cm) {
		assert(instantiated);
		dels--;
		edits--;
		penalty -= cm.del(ex);
		assert_geq(dels, 0);
		assert_geq(edits, 0);
		assert_geq(penalty, 0);
	}

	/**
	 * Charge an insertion of the given extension.
	 */
	void chargeInsert(int ex, const Scoring& cm) {
		assert(instantiated);
		ins--;
		edits--;
		penalty -= cm.ins(ex);
		assert_geq(ins, 0);
		assert_geq(edits, 0);
		assert_geq(penalty, 0);
	}
	
	/**
	 * Once the constrained area is completely explored, call this
	 * function to check whether there were *at least* as many
	 * dissimilarities as required by the constraint.  Bounds like this
	 * are helpful to resolve instances where two search roots would
	 * otherwise overlap in what alignments they can find.
	 */
	bool acceptable() {
		assert(instantiated);
		return edits   <= editsCeil &&
		       mms     <= mmsCeil   &&
		       ins     <= insCeil   &&
		       dels    <= delsCeil  &&
		       penalty <= penaltyCeil;
	}
	
	/**
	 * Instantiate a constraint w/r/t the read length and the constant
	 * and linear coefficients for the penalty function.
	 */
	static int instantiate(size_t rdlen, const SimpleFunc& func) {
		return func.f<int>((double)rdlen);
	}
	
	/**
	 * Instantiate this constraint w/r/t the read length.
	 */
	void instantiate(size_t rdlen) {
		assert(!instantiated);
		if(penFunc.initialized()) {
			penalty = Constraint::instantiate(rdlen, penFunc);
		}
		instantiated = true;
	}
	
	int edits;      // # edits permitted
	int mms;        // # mismatches permitted
	int ins;        // # insertions permitted
	int dels;       // # deletions permitted
	int penalty;    // penalty total permitted
	int editsCeil;  // <= this many edits can be left at the end
	int mmsCeil;    // <= this many mismatches can be left at the end
	int insCeil;    // <= this many inserts can be left at the end
	int delsCeil;   // <= this many deletions can be left at the end
	int penaltyCeil;// <= this much leftover penalty can be left at the end
	SimpleFunc penFunc;// penalty function; function of read len
	bool instantiated; // whether constraint is instantiated w/r/t read len
	
	//
	// Some static methods for constructing some standard Constraints
	//

	/**
	 * Construct a constraint with no edits of any kind allowed.
	 */
	static Constraint exact();
	
	/**
	 * Construct a constraint where the only constraint is a total
	 * penalty constraint.
	 */
	static Constraint penaltyBased(int pen);

	/**
	 * Construct a constraint where the only constraint is a total
	 * penalty constraint related to the length of the read.
	 */
	static Constraint penaltyFuncBased(const SimpleFunc& func);

	/**
	 * Construct a constraint where the only constraint is a total
	 * penalty constraint.
	 */
	static Constraint mmBased(int mms);

	/**
	 * Construct a constraint where the only constraint is a total
	 * penalty constraint.
	 */
	static Constraint editBased(int edits);
};

/**
 * We divide seed search strategies into three categories:
 *
 * 1. A left-to-right search where the left half of the read is
 *    constrained to match exactly and the right half is subject to
 *    some looser constraint (e.g. 1mm or 2mm)
 * 2. Same as 1, but going right to left with the exact matching half
 *    on the right.
 * 3. Inside-out search where the center half of the read is
 *    constrained to match exactly, and the extreme quarters of the
 *    read are subject to a looser constraint.
 */
enum {
	SEED_TYPE_EXACT = 1,
	SEED_TYPE_LEFT_TO_RIGHT,
	SEED_TYPE_RIGHT_TO_LEFT,
	SEED_TYPE_INSIDE_OUT
};

struct InstantiatedSeed;

/**
 * Policy dictating how to size and arrange seeds along the length of
 * the read, and what constraints to force on the zones of the seed.
 * We assume that seeds are plopped down at regular intervals from the
 * 5' to 3' ends, with the first seed flush to the 5' end.
 *
 * If the read is shorter than a single seed, one seed is used and it
 * is shrunk to accommodate the read.
 */
struct Seed {

	int len;             // length of a seed
	int type;            // dictates anchor portion, direction of search
	Constraint *overall; // for the overall alignment

	Seed() { init(0, 0, NULL); }

	/**
	 * Construct and initialize this seed with given length and type.
	 */
	Seed(int ln, int ty, Constraint* oc) {
		init(ln, ty, oc);
	}

	/**
	 * Initialize this seed with given length and type.
	 */
	void init(int ln, int ty, Constraint* oc) {
		len = ln;
		type = ty;
		overall = oc;
	}
	
	// If the seed is split into halves, we just use zones[0] and
	// zones[1]; 0 is the near half and 1 is the far half.  If the seed
	// is split into thirds (i.e. inside-out) then 0 is the center, 1
	// is the far portion on the left, and 2 is the far portion on the
	// right.
	Constraint zones[3];

	/**
	 * Once the constrained seed is completely explored, call this
	 * function to check whether there were *at least* as many
	 * dissimilarities as required by all constraints.  Bounds like this
	 * are helpful to resolve instances where two search roots would
	 * otherwise overlap in what alignments they can find.
	 */
	bool acceptable() {
		assert(overall != NULL);
		return zones[0].acceptable() &&
		       zones[1].acceptable() &&
		       zones[2].acceptable() &&
		       overall->acceptable();
	}

	/**
	 * Given a read, depth and orientation, extract a seed data structure
	 * from the read and fill in the steps & zones arrays.  The Seed
	 * contains the sequence and quality values.
	 */
	bool instantiate(
		const Read& read,
		const BTDnaString& seq, // already-extracted seed sequence
		const BTString& qual,   // already-extracted seed quality sequence
		const Scoring& pens,
		int depth,
		int seedoffidx,
		int seedtypeidx,
		bool fw,
		InstantiatedSeed& si) const;

	/**
	 * Return a list of Seed objects encapsulating
	 */
	static void mmSeeds(
		int mms,
		int ln,
		EList<Seed>& pols,
		Constraint& oall)
	{
		if(mms == 0) {
			zeroMmSeeds(ln, pols, oall);
		} else if(mms == 1) {
			oneMmSeeds(ln, pols, oall);
		} else if(mms == 2) {
			twoMmSeeds(ln, pols, oall);
		} else throw 1;
	}
	
	static void zeroMmSeeds(int ln, EList<Seed>&, Constraint&);
	static void oneMmSeeds (int ln, EList<Seed>&, Constraint&);
	static void twoMmSeeds (int ln, EList<Seed>&, Constraint&);
};

/**
 * An instantiated seed is a seed (perhaps modified to fit the read)
 * plus all data needed to conduct a search of the seed.
 */
struct InstantiatedSeed {

	InstantiatedSeed() : steps(AL_CAT), zones(AL_CAT) { }

	// Steps map.  There are as many steps as there are positions in
	// the seed.  The map is a helpful abstraction because we sometimes
	// visit seed positions in an irregular order (e.g. inside-out
	// search).
	EList<int> steps;

	// Zones map.  For each step, records what constraint to charge an
	// edit to.  The first entry in each pair gives the constraint for
	// non-insert edits and the second entry in each pair gives the
	// constraint for insert edits.  If the value stored is negative,
	// this indicates that the zone is "closed out" after this
	// position, so zone acceptility should be checked.
	EList<pair<int, int> > zones;

	// Nucleotide sequence covering the seed, extracted from read
	BTDnaString *seq;
	
	// Quality sequence covering the seed, extracted from read
	BTString *qual;
	
	// Initial constraints governing zones 0, 1, 2.  We precalculate
	// the effect of Ns on these.
	Constraint cons[3];
	
	// Overall constraint, tailored to the read length.
	Constraint overall;
	
	// Maximum number of positions that the aligner may advance before
	// its first step.  This lets the aligner know whether it can use
	// the ftab or not.
	int maxjump;
	
	// Offset of seed from 5' end of read
	int seedoff;

	// Id for seed offset; ids are such that the smallest index is the
	// closest to the 5' end and consecutive ids are adjacent (i.e.
	// there are no intervening offsets with seeds)
	int seedoffidx;
	
	// Type of seed (left-to-right, etc)
	int seedtypeidx;
	
	// Seed comes from forward-oriented read?
	bool fw;
	
	// Filtered out due to the pattern of Ns present.  If true, this
	// seed should be ignored by searchAllSeeds().
	bool nfiltered;
	
	// Seed this was instantiated from
	Seed s;
	
	/**
	 * Check that InstantiatedSeed is internally consistent.
	 */
	bool repOk() const {
		return true;
	}
};

/**
 * Data structure for holding all of the seed hits associated with a read.  All
 * the seed hits for a given read are encapsulated in a single QVal object.  A
 * QVal refers to a range of values in the qlist, where each qlist value is a 
 * BW range and a slot to hold the hit's suffix array offset.  QVals are kept
 * in two lists (hitsFw_ and hitsRc_), one for seeds on the forward read strand,
 * one for seeds on the reverse read strand.  The list is indexed by read
 * offset index (e.g. 0=closest-to-5', 1=second-closest, etc).
 *
 * An assumption behind this data structure is that all the seeds are found
 * first, then downstream analyses try to extend them.  In between finding the
 * seed hits and extending them, the sort() member function is called, which
 * ranks QVals according to the order they should be extended.  Right now the
 * policy is that QVals with fewer elements (hits) should be tried first.
 */
class SeedResults {

public:
	SeedResults() :
		seqFw_(AL_CAT),
		seqRc_(AL_CAT),
		qualFw_(AL_CAT),
		qualRc_(AL_CAT),
		hitsFw_(AL_CAT),
		hitsRc_(AL_CAT),
		isFw_(AL_CAT),
		isRc_(AL_CAT),
		sortedFw_(AL_CAT),
		sortedRc_(AL_CAT),
		offIdx2off_(AL_CAT),
		rankOffs_(AL_CAT),
		rankFws_(AL_CAT)
	{
		clear();
	}

	/**
	 * Set the appropriate element of either hitsFw_ or hitsRc_ to the given
	 * QVal.  A QVal encapsulates all the BW ranges for reference substrings 
	 * that are within some distance of the seed string.
	 */
	void add(
		const QVal& qv,           // range of ranges in cache
		const AlignmentCache& ac, // cache
		uint32_t seedIdx,         // seed index (from 5' end)
		bool     seedFw)          // whether seed is from forward read
	{
		assert(qv.repOk(ac));
		assert(repOk(&ac));
		assert_lt(seedIdx, hitsFw_.size());
		assert_gt(numOffs_, 0); // if this fails, probably failed to call reset
		if(qv.empty()) return;
		if(seedFw) {
			assert(!hitsFw_[seedIdx].valid());
			hitsFw_[seedIdx] = qv;
			numEltsFw_ += qv.numElts();
			numRangesFw_ += qv.numRanges();
			if(qv.numRanges() > 0) nonzFw_++;
		} else {
			assert(!hitsRc_[seedIdx].valid());
			hitsRc_[seedIdx] = qv;
			numEltsRc_ += qv.numElts();
			numRangesRc_ += qv.numRanges();
			if(qv.numRanges() > 0) nonzRc_++;
		}
		numElts_ += qv.numElts();
		numRanges_ += qv.numRanges();
		if(qv.numRanges() > 0) nonzTot_++;
		assert(repOk(&ac));
	}

	/**
	 * Clear buffered seed hits and state.  Set the number of seed
	 * offsets and the read.
	 */
	void reset(
		const Read& read,
		const EList<uint32_t>& offIdx2off,
		size_t numOffs)
	{
		assert_gt(numOffs, 0);
		clear();
		numOffs_ = numOffs;
		seqFw_.resize(numOffs_);
		seqRc_.resize(numOffs_);
		qualFw_.resize(numOffs_);
		qualRc_.resize(numOffs_);
		hitsFw_.resize(numOffs_);
		hitsRc_.resize(numOffs_);
		isFw_.resize(numOffs_);
		isRc_.resize(numOffs_);
		sortedFw_.resize(numOffs_);
		sortedRc_.resize(numOffs_);
		offIdx2off_ = offIdx2off;
		for(size_t i = 0; i < numOffs_; i++) {
			sortedFw_[i] = sortedRc_[i] = false;
			hitsFw_[i].reset();
			hitsRc_[i].reset();
			isFw_[i].clear();
			isRc_[i].clear();
		}
		read_ = &read;
		sorted_ = false;
	}
	
	/**
	 * Clear buffered seed hits and state.
	 */
	void clear() {
		sortedFw_.clear();
		sortedRc_.clear();
		rankOffs_.clear();
		rankFws_.clear();
		nonzTot_ = 0;
		nonzFw_ = 0;
		nonzRc_ = 0;
		numRanges_ = 0;
		numElts_ = 0;
		numRangesFw_ = 0;
		numEltsFw_ = 0;
		numRangesRc_ = 0;
		numEltsRc_ = 0;
		read_ = NULL;
		exactTopFw_ = 0;
		exactBotFw_ = 0;
		exactFw_ = false;
		exactTopRc_ = 0;
		exactBotRc_ = 0;
		exactRc_ = false;
		assert(empty());
	}

	/**
	 * Install an exact end-to-end alignment for the whole read.
	 */
	void installExactHit(uint32_t top, uint32_t bot, bool fw) {
		if(fw) {
			exactTopFw_ = top;
			exactBotFw_ = bot;
			exactFw_ = true;
		} else {
			exactTopRc_ = top;
			exactBotRc_ = bot;
			exactRc_ = true;
		}
	}

	/**
	 * Return the number of ranges being held.
	 */
	size_t numRanges() const { return numRanges_; }

	/**
	 * Return the number of elements being held.
	 */
	size_t numElts() const { return numElts_; }

	/**
	 * Return the number of ranges being held for seeds on the forward
	 * read strand.
	 */
	size_t numRangesFw() const { return numRangesFw_; }

	/**
	 * Return the number of elements being held for seeds on the
	 * forward read strand.
	 */
	size_t numEltsFw() const { return numEltsFw_; }

	/**
	 * Return the number of ranges being held for seeds on the
	 * reverse-complement read strand.
	 */
	size_t numRangesRc() const { return numRangesRc_; }

	/**
	 * Return the number of elements being held for seeds on the
	 * reverse-complement read strand.
	 */
	size_t numEltsRc() const { return numEltsRc_; }
	
	/**
	 * Return true iff there are 0 hits being held.
	 */
	bool empty() const { return numRanges() == 0; }
	
	/**
	 * Get the QVal representing all the reference hits for the given
	 * orientation and seed offset index.
	 */
	const QVal& hitsAtOffIdx(bool fw, size_t seedoffidx) const {
		assert_lt(seedoffidx, numOffs_);
		assert(repOk(NULL));
		return fw ? hitsFw_[seedoffidx] : hitsRc_[seedoffidx];
	}

	/**
	 * Get the Instantiated seeds for the given orientation and offset.
	 */
	EList<InstantiatedSeed>& instantiatedSeeds(bool fw, size_t seedoffidx) {
		assert_lt(seedoffidx, numOffs_);
		assert(repOk(NULL));
		return fw ? isFw_[seedoffidx] : isRc_[seedoffidx];
	}
	
	/**
	 * Return the number of different seed offsets possible.
	 */
	size_t numOffs() const { return numOffs_; }
	
	/**
	 * Return the read from which seeds were extracted, aligned.
	 */
	const Read& read() const { return *read_; }
	
	/**
	 * Check that this SeedResults is internally consistent.
	 */
	bool repOk(
		const AlignmentCache* ac,
		bool requireInited = false) const
	{
		if(requireInited) {
			assert(read_ != NULL);
		}
		if(read_ != NULL) {
			assert_gt(numOffs_, 0);
			assert_eq(numOffs_, hitsFw_.size());
			assert_eq(numOffs_, hitsRc_.size());
			assert_leq(numRanges_, numElts_);
			assert_leq(nonzTot_, numRanges_);
			size_t nonzs = 0;
			for(int fw = 0; fw <= 1; fw++) {
				const EList<QVal>& rrs = (fw ? hitsFw_ : hitsRc_);
				for(size_t i = 0; i < numOffs_; i++) {
					if(rrs[i].valid()) {
						if(rrs[i].numRanges() > 0) nonzs++;
						if(ac != NULL) {
							assert(rrs[i].repOk(*ac));
						}
					}
				}
			}
			assert_eq(nonzs, nonzTot_);
			assert(!sorted_ || nonzTot_ == rankFws_.size());
			assert(!sorted_ || nonzTot_ == rankOffs_.size());
		}
		return true;
	}
	
	/**
	 * Populate rankOffs_ and rankFws_ with the list of QVals that need to be
	 * examined for this SeedResults, in order.  The order is ascending by
	 * number of elements, so QVals with fewer elements (i.e. seed sequences
	 * that are more unique) will be tried first and QVals with more elements
	 * (i.e. seed sequences
	 */
	void rankSeedHits(RandomSource& rnd) {
		while(rankOffs_.size() < nonzTot_) {
			uint32_t minsz = 0xffffffff;
			uint32_t minidx = 0;
			bool minfw = true;
			// Rank seed-hit positions in ascending order by number of elements
			// in all BW ranges
			bool rb = rnd.nextBool();
			assert(rb == 0 || rb == 1);
			for(int fwi = 0; fwi <= 1; fwi++) {
				bool fw = (fwi == (rb ? 1 : 0));
				EList<QVal>& rrs = (fw ? hitsFw_ : hitsRc_);
				EList<bool>& sorted = (fw ? sortedFw_ : sortedRc_);
				uint32_t i = (rnd.nextU32() % (uint32_t)numOffs_);
				for(uint32_t ii = 0; ii < numOffs_; ii++) {
					if(rrs[i].valid() &&         // valid QVal
					   rrs[i].numElts() > 0 &&   // non-empty
					   !sorted[i] &&             // not already sorted
					   rrs[i].numElts() < minsz) // least elts so far?
					{
						minsz = rrs[i].numElts();
						minidx = i;
						minfw = (fw == 1);
					}
					if((++i) == numOffs_) {
						i = 0;
					}
				}
			}
			assert_neq(0xffffffff, minsz);
			if(minfw) {
				sortedFw_[minidx] = true;
			} else {
				sortedRc_[minidx] = true;
			}
			rankOffs_.push_back(minidx);
			rankFws_.push_back(minfw);
		}
		assert_eq(rankOffs_.size(), rankFws_.size());
		sorted_ = true;
	}

	/**
	 * Return the number of orientation/offsets into the read that have
	 * at least one seed hit.
	 */
	size_t nonzeroOffsets() const {
		assert(!sorted_ || nonzTot_ == rankFws_.size());
		assert(!sorted_ || nonzTot_ == rankOffs_.size());
		return nonzTot_;
	}
	
	/**
	 * Return true iff all seeds hit (all have at least one range).
	 */
	bool allSeedsHit() const {
		return nonzeroOffsets() == numOffs();
	}

	/**
	 * Return the number of offsets into the forward read that have at
	 * least one seed hit.
	 */
	size_t nonzeroOffsetsFw() const {
		return nonzFw_;
	}
	
	/**
	 * Return the number of offsets into the reverse-complement read
	 * that have at least one seed hit.
	 */
	size_t nonzeroOffsetsRc() const {
		return nonzRc_;
	}

	/**
	 * Return a QVal of seed hits of the given rank 'r'.  'offidx' gets the id
	 * of the offset from 5' from which it was extracted (0 for the 5-most
	 * offset, 1 for the next closes to 5', etc).  'off' gets the offset from
	 * the 5' end.  'fw' gets true iff the seed was extracted from the forward
	 * read.
	 */
	const QVal& hitsByRank(
		size_t    r,      // in
		uint32_t& offidx, // out
		uint32_t& off,    // out
		bool&     fw,     // out
		uint32_t& seedlen)// out
	{
		assert(sorted_);
		assert_lt(r, nonzTot_);
		if(rankFws_[r]) {
			fw = true;
			offidx = rankOffs_[r];
			assert_lt(offidx, offIdx2off_.size());
			off = offIdx2off_[offidx];
			seedlen = (uint32_t)seqFw_[rankOffs_[r]].length();
			return hitsFw_[rankOffs_[r]];
		} else {
			fw = false;
			offidx = rankOffs_[r];
			assert_lt(offidx, offIdx2off_.size());
			off = offIdx2off_[offidx];
			seedlen = (uint32_t)seqRc_[rankOffs_[r]].length();
			return hitsRc_[rankOffs_[r]];
		}
	}

	/**
	 * Return an EList of seed hits of the given rank.
	 */
	const BTDnaString& seqByRank(size_t r) {
		assert(sorted_);
		assert_lt(r, nonzTot_);
		return rankFws_[r] ? seqFw_[rankOffs_[r]] : seqRc_[rankOffs_[r]];
	}

	/**
	 * Return an EList of seed hits of the given rank.
	 */
	const BTString& qualByRank(size_t r) {
		assert(sorted_);
		assert_lt(r, nonzTot_);
		return rankFws_[r] ? qualFw_[rankOffs_[r]] : qualRc_[rankOffs_[r]];
	}
	
	/**
	 * Return the list of extracted seed sequences for seeds on either
	 * the forward or reverse strand.
	 */
	EList<BTDnaString>& seqs(bool fw) { return fw ? seqFw_ : seqRc_; }

	/**
	 * Return the list of extracted quality sequences for seeds on
	 * either the forward or reverse strand.
	 */
	EList<BTString>& quals(bool fw) { return fw ? qualFw_ : qualRc_; }

	/**
	 * Print a pretty, short summary of these seed results.
	 */
	void printSummary(std::ostream& os) {
		os << "  # positions: "           << numOffs_   << std::endl
		   << "  # non-empty positions: " << nonzTot_   << std::endl
		   << "  # BW ranges found: "     << numRanges_ << std::endl
		   << "  # BW elts found: "       << numElts_   << std::endl
		   << "  sorted?: "               << sorted_    << std::endl;
		if(sorted_) {
			os << "  Sorted offsets:" << std::endl;
			for(size_t i = 0; i < rankOffs_.size(); i++) {
				os << "    " << i << ": " << rankOffs_[i] << "," << rankFws_[i] << std::endl;
			}
		}
	}
	
	/**
	 * Return information about any exact end-to-end alignments found.
	 */
	void exactE2EHits(
		uint32_t& top_fw,
		uint32_t& bot_fw,
		uint32_t& top_rc,
		uint32_t& bot_rc) const
	{
		if(exactFw_) {
			top_fw = exactTopFw_;
			bot_fw = exactBotFw_;
		} else {
			top_fw = bot_fw = 0;
		}
		if(exactRc_) {
			top_rc = exactTopRc_;
			bot_rc = exactBotRc_;
		} else {
			top_rc = bot_rc = 0;
		}
	}
	
	/**
	 * Return the number of distinct exact end-to-end hits found.
	 */
	size_t numExactE2eHits() const {
		return (exactFw_ ? (exactBotFw_ - exactTopFw_) : 0) +
		       (exactRc_ ? (exactBotRc_ - exactTopRc_) : 0);
	}
	
	/**
	 * Return the length of the read that yielded the seed hits.
	 */
	size_t readLength() const {
		assert(read_ != NULL);
		return read_->length();
	}

protected:

	// As seed hits and edits are added they're sorted into these
	// containers
	EList<BTDnaString>  seqFw_;       // seqs for seeds from forward read
	EList<BTDnaString>  seqRc_;       // seqs for seeds from revcomp read
	EList<BTString>     qualFw_;      // quals for seeds from forward read
	EList<BTString>     qualRc_;      // quals for seeds from revcomp read
	EList<QVal>         hitsFw_;      // hits for forward read
	EList<QVal>         hitsRc_;      // hits for revcomp read
	EList<EList<InstantiatedSeed> > isFw_; // hits for forward read
	EList<EList<InstantiatedSeed> > isRc_; // hits for revcomp read
	EList<bool>         sortedFw_;    // true iff fw QVal was sorted/ranked
	EList<bool>         sortedRc_;    // true iff rc QVal was sorted/ranked
	size_t              nonzTot_;     // # offsets with non-zero size
	size_t              nonzFw_;      // # offsets into fw read with non-0 size
	size_t              nonzRc_;      // # offsets into rc read with non-0 size
	size_t              numRanges_;   // # ranges added
	size_t              numElts_;     // # elements added
	size_t              numRangesFw_; // # ranges added for fw seeds
	size_t              numEltsFw_;   // # elements added for fw seeds
	size_t              numRangesRc_; // # ranges added for rc seeds
	size_t              numEltsRc_;   // # elements added for rc seeds

	EList<uint32_t>     offIdx2off_;// map from offset indexes to offsets from 5' end

	// When the sort routine is called, the seed hits collected so far
	// are sorted into another set of containers that allow easy access
	// to hits from the lowest-ranked offset (the one with the fewest
	// BW elements) to the greatest-ranked offset.  Offsets with 0 hits
	// are ignored.
	EList<uint32_t>     rankOffs_;  // sorted offests of seeds to try
	EList<bool>         rankFws_;   // sorted orientations assoc. with rankOffs_
	bool                sorted_;    // true if sort() called since last reset
	
	// These fields set once per read
	size_t              numOffs_;   // # different seed offsets possible
	const Read*         read_;      // read from which seeds were extracted
	
	uint32_t            exactTopFw_;
	uint32_t            exactBotFw_;
	bool                exactFw_;
	uint32_t            exactTopRc_;
	uint32_t            exactBotRc_;
	bool                exactRc_;
};

/**
 * A set of counters for characterizing the work done by the seed
 * aligner.
 */
struct SACounters {
	uint64_t seed;      // seeds searched
	uint64_t ftab;      // ftab jumps
	uint64_t fchr;      // fchr jumps
	uint64_t match;     // match advances
	uint64_t matchd[4]; // match advances at depth 0, 1, 2, >=3
	uint64_t edit;      // edit advances
	uint64_t editd[4];  // edit advances at depth 0, 1, 2, >=3
	int hits;           // # valid alignments found
	int maxDepth;       // maximum recursion depth
	
	/**
	 * Set all counters to 0;
	 */
	void reset() {
		hits = 0;
		seed = 0;
		ftab = 0;
		fchr = 0;
		maxDepth = 0;
		match = 0;
		matchd[0] = matchd[1] = matchd[2] = matchd[3] = 0;
		edit = 0;
		editd[0] = editd[1] = editd[2] = editd[3] = 0;
	}
};

/**
 * Abstract parent class for encapsulating SeedAligner actions.
 */
struct SAAction {
	
	SAAction() :
		type(0), seed(0), seedoff(0), pos(0), ltr(true), len(0), depth(0), edit() { }
	SAAction(int sd, int sdo, int ps, bool lr, int ln, int dp, Edit e) :
		type(0), seed(sd), seedoff(sdo), pos(ps), ltr(lr), len(ln), depth(dp), edit(e) { }
	
	int  type;    // type
	int  seed;    // seed
	int  seedoff; // offset of seed
	int  pos;     // position before jump
	bool ltr;     // direction of jump
	int  len;     // length
	int  depth;   // depth of recursion stack
	Edit edit;    // edit performed
};

/**
 * Abstract parent for a class with a method that gets passed every
 * seed hit.
 */
class SeedHitSink {
public:
	SeedHitSink() { MUTEX_INIT(lock_); }
	virtual ~SeedHitSink() { }

	/**
	 * Grab the lock and call abstract member reportSeedHitImpl()
	 */
	virtual void reportSeedHit(
		const Read& rd,
		const BTDnaString& seedseq)
	{
		ThreadSafe(&this->lock_);
		reportSeedHitImpl(rd, seedseq);
	}

protected:

	virtual void reportSeedHitImpl(const Read& rd, const BTDnaString& seedseq) = 0;
	MUTEX_T lock_;
};

/**
 * Write each hit to an output stream using a simple record-per-line
 * tab-delimited format.
 */
class StreamTabSeedHitSink : public SeedHitSink {
public:
	StreamTabSeedHitSink(std::ostream& os) : SeedHitSink(), os_(os) { }
	virtual ~StreamTabSeedHitSink() { }
protected:
	virtual void reportSeedHitImpl(
		const Read& rd,
		const BTDnaString& seedseq)
	{
		os_ << rd.patFw  << "\t"
		    << rd.qual   << "\t"
		    << seedseq   << "\n";
		  //  << h.topf()  << "\t"
		//	<< h.botf()  << "\t"
		//	<< h.topb()  << "\t"
		//	<< h.editn() << "\n"; // avoid 'endl' b/c flush is unnecessary
	}
	std::ostream& os_;
};

/**
 * Abstract parent for a class with a method that gets passed every
 * set of counters for every read.
 */
class SeedCounterSink {
public:
	SeedCounterSink() { MUTEX_INIT(lock_); }
	virtual ~SeedCounterSink() { }

	/**
	 * Grab the lock and call abstract member reportCountersImpl()
	 */
	virtual void reportCounters(const Read& rd, const SACounters& c) {
		ThreadSafe(&this->lock_);
		reportCountersImpl(rd, c);
	}

protected:

	virtual void reportCountersImpl(const Read& rd, const SACounters& c) = 0;

	MUTEX_T lock_;
};

/**
 * Write each per-read set of counters to an output stream using a
 * simple record-per-line tab-delimited format.
 */
class StreamTabSeedCounterSink : public SeedCounterSink {
public:
	StreamTabSeedCounterSink(std::ostream& os) : SeedCounterSink(), os_(os) { }
	virtual ~StreamTabSeedCounterSink() { }
protected:
	virtual void reportCountersImpl(const Read& rd, const SACounters& c) {
	// # Plot number of depth-0 and depth-1 matches for reads that had 0 or >0 seed hits
	// plot(cnts[,6][cnts[,14] > 0], cnts[,7][cnts[,14] > 0], col="blue")
	// points(cnts[,6][cnts[,14] == 0], cnts[,7][cnts[,14] == 0], col="red")

	// # Plot number of depth-0 and depth-1 matches for reads that had 0 or >0 seed hits
	// bwops <- apply(cnts[,6:13], 1, sum)
	// plot(bwops, cnts[,14], xlab="BW ops", ylab="Number of seed hits")
	// (above is close to a stright line, bowed down in the middle somewhat)
	// bwopsnz <- bwops[bwops > 0]
	// bwopsnz <- sort(bwopsnz, decreasing=T)
	// prefix.sum <- function(x) {
	//   for(i in 1:(length(x)-1)) {
	//     x[i+1] <- x[i+1] + x[i]
	//   }
	//   x
	// }
	// effort <- prefix.sum(bwopsnz)/sum(bwopsnz)
	// plot(effort, xlab="Descending BW ops rank", ylab="Cumulative fraction of BW op effort")
		os_ << rd.patFw    << "\t" // 1: read sequence
		    << rd.qual     << "\t" // 2: quality sequence
		    << c.seed      << "\t" // 3: # seeds searched
			<< c.ftab      << "\t" // 4: # times ftab queried
			<< c.fchr      << "\t" // 5: # times fchr queried
			<< c.matchd[0] << "\t" // 6: # match advances at depth 0
			<< c.matchd[1] << "\t" // 7: # match advances at depth 1
			<< c.matchd[2] << "\t" // 8: # match advances at depth 2
			<< c.matchd[3] << "\t" // 9: # match advances at depth >=3
			<< c.editd[0]  << "\t" // 10: # match advances at depth 0
			<< c.editd[1]  << "\t" // 11: # match advances at depth 1
			<< c.editd[2]  << "\t" // 12: # match advances at depth 2
			<< c.editd[3]  << "\t" // 13: # match advances at depth >=3
			<< c.hits      << "\t" // 14: # seed hits
		    << c.maxDepth  << "\n";// 15: max depth
			// avoid 'endl' b/c flush is unnecessary
	}
	std::ostream& os_;
};

/**
 * Abstract parent for a class with a method that gets passed every
 * seed hit.
 */
class SeedActionSink {
public:
	SeedActionSink() { MUTEX_INIT(lock_); }
	virtual ~SeedActionSink() { }

	/**
	 * Grab the lock and call abstract member reportActionsImpl()
	 */
	virtual void reportActions(const Read& rd, const EList<SAAction>& a) {
		ThreadSafe(&this->lock_);
		reportActionsImpl(rd, a);
	}

protected:
	virtual void reportActionsImpl(const Read& rd, const EList<SAAction>& a) = 0;

	MUTEX_T lock_;
};

/**
 * Write each action to an output stream using a simple record-per-line
 * tab-delimited format.
 */
class StreamTabSeedActionSink : public SeedActionSink {
public:
	StreamTabSeedActionSink(std::ostream& os) : SeedActionSink(), os_(os) { }
	virtual ~StreamTabSeedActionSink() { }
protected:
	virtual void reportActionsImpl(const Read& rd, const EList<SAAction>& a) {
		for(size_t i = 0; i < a.size(); i++) {
			os_ << rd.patFw     << "\t"
				<< rd.qual      << "\t"
				// Omit jump-related fields
				<< a[i].pos     << "\t"
				<< a[i].type    << "\t"
				<< a[i].seed    << "\t"
				<< a[i].seedoff << "\t"
				<< a[i].depth   << "\n"; // avoid 'endl' b/c flush is unnecessary
		}
	}
	std::ostream& os_;
};

// Forward decl
class Ebwt;
class SideLocus;
class ReadCounterSink;

/**
 * Encapsulates a sumamry of what the searchAllSeeds aligner did.
 */
struct SeedSearchMetrics {

	SeedSearchMetrics() { reset(); MUTEX_INIT(lock); }

	/**
	 * Merge this metrics object with the given object, i.e., sum each
	 * category.  This is the only safe way to update a
	 * SeedSearchMetrics object shread by multiple threads.
	 */
	void merge(const SeedSearchMetrics& m, bool getLock = false) {
		ThreadSafe ts(&lock, getLock);
		seedsearch += m.seedsearch;
		possearch  += m.possearch;
		intrahit   += m.intrahit;
		interhit   += m.interhit;
		filteredseed+= m.filteredseed;
		ooms       += m.ooms;
		bwops      += m.bwops;
		bweds      += m.bweds;
	}
	
	/**
	 * Set all counters to 0.
	 */
	void reset() {
		seedsearch =
		possearch =
		intrahit =
		interhit =
		filteredseed =
		ooms =
		bwops =
		bweds = 0;
	}

	uint64_t seedsearch; // # times aligner executed the search strategy in an InstantiatedSeed
	uint64_t possearch;  // # offsets where aligner executed at least 1 strategy
	uint64_t intrahit;   // # offsets where current-read cache provided answer
	uint64_t interhit;   // # offsets where across-read cache provided answer
	uint64_t filteredseed;// # seed instantiations skipped due to Ns
	uint64_t ooms;       // out-of-memory errors
	uint64_t bwops;      // Burrows-Wheeler operations
	uint64_t bweds;      // Burrows-Wheeler edits
	MUTEX_T  lock;
};

/**
 * Given an index and a seeding scheme, searches for seed hits.
 */
class SeedAligner {

public:
	
	/**
	 * Initialize with index.
	 */
	SeedAligner() : edits_(AL_CAT), offIdx2off_(AL_CAT) { }

	/**
	 * Given a read and a few coordinates that describe a substring of the
	 * read (or its reverse complement), fill in 'seq' and 'qual' objects
	 * with the seed sequence and qualities.
	 */
	void instantiateSeq(
		const Read& read, // input read
		BTDnaString& seq, // output sequence
		BTString& qual,   // output qualities
		int len,          // seed length
		int depth,        // seed's 0-based offset from 5' end
		bool fw) const;   // seed's orientation

	/**
	 * Iterate through the seeds that cover the read and initiate a
	 * search for each seed.
	 */
	std::pair<int, int> instantiateSeeds(
		const EList<Seed>& seeds,   // search seeds
		int per,                    // interval between seeds
		const Read& read,           // read to align
		const Scoring& pens,        // scoring scheme
		bool nofw,                  // don't align forward read
		bool norc,                  // don't align revcomp read
		AlignmentCacheIface& cache, // holds some seed hits from previous reads
		SeedResults& sr,            // holds all the seed hits
		SeedSearchMetrics& met);    // metrics

	/**
	 * Iterate through the seeds that cover the read and initiate a
	 * search for each seed.
	 */
	void searchAllSeeds(
		const EList<Seed>& seeds,   // search seeds
		const Ebwt* ebwtFw,         // BWT index
		const Ebwt* ebwtBw,         // BWT' index
		const Read& read,           // read to align
		const Scoring& pens,        // scoring scheme
		AlignmentCacheIface& cache, // local seed alignment cache
		SeedResults& hits,          // holds all the seed hits
		SeedSearchMetrics& met);    // metrics

	/**
	 * Search for end-to-end exact hit for read.  Return true iff one is found.
	 */
	bool exactSearch(
		const Ebwt* ebwtFw,         // BWT index
		const Read& read,           // read to align
		bool nofw,                  // don't align forward read
		bool norc,                  // don't align revcomp read
		SeedResults& hits,          // holds all the seed hits (and exact hit)
		SeedSearchMetrics& met);    // metrics

protected:

	/**
	 * Report a seed hit found by searchSeedBi() by adding it to the back
	 * of the hits_ list and adding its edits to the back of the edits_
	 * list.
	 */
	bool reportHit(
		uint32_t topf,         // top in BWT
		uint32_t botf,         // bot in BWT
		uint32_t topb,         // top in BWT'
		uint32_t botb,         // bot in BWT'
		uint16_t len,          // length of hit
		DoublyLinkedList<Edit> *prevEdit);  // previous edit
	
	/**
	 * Given an instantiated seed (in s_ and other fields), search
	 */
	bool searchSeedBi();
	
	/**
	 * Main, recursive implementation of the seed search.
	 */
	bool searchSeedBi(
		int step,              // depth into steps_[] array
		int depth,             // recursion depth
		uint32_t topf,         // top in BWT
		uint32_t botf,         // bot in BWT
		uint32_t topb,         // top in BWT'
		uint32_t botb,         // bot in BWT'
		SideLocus tloc,        // locus for top (perhaps unititialized)
		SideLocus bloc,        // locus for bot (perhaps unititialized)
		Constraint c0,         // constraints to enforce in seed zone 0
		Constraint c1,         // constraints to enforce in seed zone 1
		Constraint c2,         // constraints to enforce in seed zone 2
		Constraint overall,    // overall constraints
		DoublyLinkedList<Edit> *prevEdit);  // previous edit
	
	/**
	 * Get tloc and bloc ready for the next step.
	 */
	inline void nextLocsBi(
		SideLocus& tloc,            // top locus
		SideLocus& bloc,            // bot locus
		uint32_t topf,              // top in BWT
		uint32_t botf,              // bot in BWT
		uint32_t topb,              // top in BWT'
		uint32_t botb,              // bot in BWT'
		int step);                  // step to get ready for
	
	// Following are set in searchAllSeeds then used by searchSeed()
	// and other protected members.
	const Ebwt* ebwtFw_;       // forward index (BWT)
	const Ebwt* ebwtBw_;       // backward/mirror index (BWT')
	const Scoring* sc_;        // scoring scheme
	const InstantiatedSeed* s_;// current instantiated seed
	const Read* read_;         // read whose seeds are currently being aligned
	EList<SeedHitSink*> *sinks_;// if non-NULL, list of sinks to send hits to
	EList<SeedCounterSink*>* counterSinks_; // if non-NULL, list of sinks to send SACounters to
	EList<SeedActionSink*>* actionSinks_;   // if non-NULL, list of sinks to send SAActions to
	const BTDnaString* seq_;   // sequence of current seed
	const BTString* qual_;     // quality string for current seed
	EList<Edit> edits_;        // temporary place to sort edits
	AlignmentCacheIface *ca_;  // local alignment cache for seed alignments
	EList<uint32_t> offIdx2off_;// offset idx to read offset map, set up instantiateSeeds()
	uint64_t bwops_;           // Burrows-Wheeler operations
	uint64_t bwedits_;         // Burrows-Wheeler edits
	BTDnaString tmprfdnastr_;  // used in reportHit
	
	ASSERT_ONLY(ESet<BTDnaString> hits_); // Ref hits so far for seed being aligned
	ASSERT_ONLY(BTDnaString tmpdnastr_);
};

#endif /*ALIGNER_SEED_H_*/
