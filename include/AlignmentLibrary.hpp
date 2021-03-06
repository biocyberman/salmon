#ifndef ALIGNMENT_LIBRARY_HPP
#define ALIGNMENT_LIBRARY_HPP

extern "C" {
#include "io_lib/scram.h"
#include "io_lib/os.h"
#undef max
#undef min
}

// Our includes
#include "ClusterForest.hpp"
#include "Transcript.hpp"
#include "BAMQueue.hpp"
#include "SalmonUtils.hpp"
#include "LibraryFormat.hpp"
#include "SalmonOpts.hpp"
#include "FragmentLengthDistribution.hpp"
#include "FragmentStartPositionDistribution.hpp"
#include "AlignmentGroup.hpp"
#include "ErrorModel.hpp"
#include "AlignmentModel.hpp"
#include "FASTAParser.hpp"
#include "concurrentqueue.h"
#include "EquivalenceClassBuilder.hpp"
#include "SpinLock.hpp" // RapMap's with try_lock
#include "ReadKmerDist.hpp"

// Boost includes
#include <boost/filesystem.hpp>

// Standard includes
#include <vector>
#include <memory>
#include <functional>

template <typename T>
class NullFragmentFilter;

/**
  *  This class represents a library of alignments used to quantify
  *  a set of target transcripts.  The AlignmentLibrary contains info
  *  about both the alignment file and the target sequence (transcripts).
  *  It is used to group them together and track information about them
  *  during the quantification procedure.
  */
template <typename FragT>
class AlignmentLibrary {

    public:

    AlignmentLibrary(std::vector<boost::filesystem::path>& alnFiles,
                     const boost::filesystem::path& transcriptFile,
                     LibraryFormat libFmt,
                     SalmonOpts& salmonOpts) :
        alignmentFiles_(alnFiles),
        transcriptFile_(transcriptFile),
        libFmt_(libFmt),
        transcripts_(std::vector<Transcript>()),
    	fragStartDists_(5),
        seqBiasModel_(1.0),
    	eqBuilder_(salmonOpts.jointLog),
        quantificationPasses_(0),
        expectedBias_(constExprPow(4, readBias_.getK()), 1.0),
        expectedGC_(101, 1.0),
        observedGC_(101, 1e-5) {
            namespace bfs = boost::filesystem;

            // Make sure the alignment file exists.
            for (auto& alignmentFile : alignmentFiles_) {
                if (!bfs::exists(alignmentFile)) {
                    std::stringstream ss;
                    ss << "The provided alignment file: " << alignmentFile <<
                        " does not exist!\n";
                    throw std::invalid_argument(ss.str());
                }
            }


            // Make sure the transcript file exists.
            if (!bfs::exists(transcriptFile_)) {
                std::stringstream ss;
                ss << "The provided transcript file: " << transcriptFile_ <<
                    " does not exist!\n";
                throw std::invalid_argument(ss.str());
            }

            // The alignment file existed, so create the alignment queue
            size_t numParseThreads = salmonOpts.numParseThreads;
            std::cerr << "parseThreads = " << numParseThreads << "\n";
            bq = std::unique_ptr<BAMQueue<FragT>>(new BAMQueue<FragT>(alnFiles, libFmt_, numParseThreads,
                                                                      salmonOpts.mappingCacheMemoryLimit));

            std::cerr << "Checking that provided alignment files have consistent headers . . . ";
            if (! salmon::utils::headersAreConsistent(bq->headers()) ) {
                std::stringstream ss;
                ss << "\nThe multiple alignment files provided had inconsistent headers.\n";
                ss << "Currently, we require that if multiple SAM/BAM files are provided,\n";
                ss << "they must have identical @SQ records.\n";
                throw std::invalid_argument(ss.str());
            }
            std::cerr << "done\n";

            SAM_hdr* header = bq->header();

            // The transcript file existed, so load up the transcripts
            double alpha = 0.005;
            for (size_t i = 0; i < header->nref; ++i) {
                transcripts_.emplace_back(i, header->ref[i].name, header->ref[i].len, alpha);
            }

            FASTAParser fp(transcriptFile.string());

            fmt::print(stderr, "Populating targets from aln = {}, fasta = {} . . .",
                       alnFiles.front(), transcriptFile_);
            fp.populateTargets(transcripts_);
	    for (auto& txp : transcripts_) {
		    // Length classes taken from
		    // ======
		    // Roberts, Adam, et al.
		    // "Improving RNA-Seq expression estimates by correcting for fragment bias."
		    // Genome Biol 12.3 (2011): R22.
		    // ======
		    // perhaps, define these in a more data-driven way
		    if (txp.RefLength <= 1334) {
			    txp.lengthClassIndex(0);
		    } else if (txp.RefLength <= 2104) {
			    txp.lengthClassIndex(0);
		    } else if (txp.RefLength <= 2988) {
			    txp.lengthClassIndex(0);
		    } else if (txp.RefLength <= 4389) {
			    txp.lengthClassIndex(0);
		    } else {
			    txp.lengthClassIndex(0);
		    }
	    }
            fmt::print(stderr, "done\n");

            // Create the cluster forest for this set of transcripts
            clusters_.reset(new ClusterForest(transcripts_.size(), transcripts_));

            // Initialize the fragment length distribution
            size_t maxFragLen = salmonOpts.fragLenDistMax;
            size_t meanFragLen = salmonOpts.fragLenDistPriorMean;
            size_t fragLenStd = salmonOpts.fragLenDistPriorSD;
            size_t fragLenKernelN = 4;
            double fragLenKernelP = 0.5;
            flDist_.reset(new
                    FragmentLengthDistribution(
                    1.0, maxFragLen,
                    meanFragLen, fragLenStd,
                    fragLenKernelN,
                    fragLenKernelP, 1)
                    );

            alnMod_.reset(new AlignmentModel(1.0, salmonOpts.numErrorBins));
            alnMod_->setLogger(salmonOpts.jointLog);

            // Start parsing the alignments
           NullFragmentFilter<FragT>* nff = nullptr;
           bool onlyProcessAmbiguousAlignments = false;
           bq->start(nff, onlyProcessAmbiguousAlignments);
        }

    EquivalenceClassBuilder& equivalenceClassBuilder() {
        return eqBuilder_;
    }

    void updateTranscriptLengthsAtomic(std::atomic<bool>& done) {
        if (sl_.try_lock()) {
            if (!done) {
                auto& fld = *(flDist_.get());
                std::vector<double> logPMF;
                size_t minVal;
                size_t maxVal;
                double logFLDMean = fld.mean();
                fld.dumpPMF(logPMF, minVal, maxVal);
                double sum = salmon::math::LOG_0;
                for (auto v : logPMF) {
                    sum = salmon::math::logAdd(sum, v);
                }
                for (auto& v : logPMF) {
                    v -= sum;
                }
                // Update the effective length of *every* transcript
                for( auto& t : transcripts_ ) {
                    t.updateEffectiveLength(logPMF, logFLDMean, minVal, maxVal);
                }
                // then declare that we are done
                done = true;
                sl_.unlock();
            }
        }
    }

    std::vector<Transcript>& transcripts() { return transcripts_; }
    const std::vector<Transcript>& transcripts() const { return transcripts_; }

    inline bool getAlignmentGroup(AlignmentGroup<FragT>*& ag) { return bq->getAlignmentGroup(ag); }

    //inline t_pool* threadPool() { return threadPool_.get(); }

    inline SAM_hdr* header() { return bq->header(); }

    inline std::vector<FragmentStartPositionDistribution>& fragmentStartPositionDistributions() {
	    return fragStartDists_;
    }

    inline FragmentLengthDistribution* fragmentLengthDistribution() const {
        return flDist_.get();
    }

    inline AlignmentModel& alignmentModel() {
        return *alnMod_.get();
    }

    SequenceBiasModel& sequenceBiasModel() {
        return seqBiasModel_;
    }

//    inline tbb::concurrent_queue<FragT*>& fragmentQueue() {
    inline tbb::concurrent_queue<FragT*>& fragmentQueue() {
        return bq->getFragmentQueue();
    }

//    inline tbb::concurrent_bounded_queue<AlignmentGroup<FragT*>*>& alignmentGroupQueue() {
    inline moodycamel::ConcurrentQueue<AlignmentGroup<FragT*>*>& alignmentGroupQueue() {
        return bq->getAlignmentGroupQueue();
    }

    inline BAMQueue<FragT>& getAlignmentGroupQueue() { return *bq.get(); }

    inline size_t upperBoundHits() { return bq->numMappedFragments(); }
    inline size_t numObservedFragments() const { return bq->numObservedFragments(); }
    inline size_t numMappedFragments() const { return bq->numMappedFragments(); }
    inline size_t numUniquelyMappedFragments() { return bq->numUniquelyMappedFragments(); }
    inline double effectiveMappingRate() const {
        return static_cast<double>(numMappedFragments()) / numObservedFragments();
    }

    //const boost::filesystem::path& alignmentFile() { return alignmentFile_; }

    ClusterForest& clusterForest() { return *clusters_.get(); }

    template <typename FilterT>
    bool reset(bool incPasses=true, FilterT filter=nullptr, bool onlyProcessAmbiguousAlignments=false) {
        namespace bfs = boost::filesystem;

        for (auto& alignmentFile : alignmentFiles_) {
            if (!bfs::is_regular_file(alignmentFile)) {
                return false;
            }
        }

        bq->reset();
        bq->start(filter, onlyProcessAmbiguousAlignments);
        if (incPasses) {
            quantificationPasses_++;
            fmt::print(stderr, "Current iteration = {}\n", quantificationPasses_);
        }
        return true;
    }

    inline LibraryFormat format() { return libFmt_; }

    void setGCFracForward(double fracForward) { gcFracFwd_ = fracForward; }

    double gcFracFwd() const { return gcFracFwd_; }
    double gcFracRC() const { return 1.0 - gcFracFwd_; }

    void setExpectedSeqBias(const std::vector<double>& expectedBiasIn) {
        expectedBias_ = expectedBiasIn;
    }

    std::vector<double>& expectedSeqBias() {
        return expectedBias_;
    }

    const std::vector<double>& expectedSeqBias() const {
        return expectedBias_;
    }

    void setExpectedGCBias(const std::vector<double>& expectedBiasIn) {
        expectedGC_ = expectedBiasIn;
    }

    std::vector<double>& expectedGCBias() {
        return expectedGC_;
    }

    const std::vector<double>& expectedGCBias() const {
        return expectedGC_;
    }

    const std::vector<double>& observedGC() const {
        return observedGC_;
    }

    std::vector<double>& observedGC() {
        return observedGC_;
    }


    ReadKmerDist<6, std::atomic<uint32_t>>& readBias() { return readBias_; }
    const ReadKmerDist<6, std::atomic<uint32_t>>& readBias() const { return readBias_; }


    private:
    /**
     * The file from which the alignments will be read.
     * This can be a SAM or BAM file, and can be a regular
     * file or a fifo.
     */
    std::vector<boost::filesystem::path> alignmentFiles_;
    /**
     * The file from which the transcripts are read.
     * This is expected to be a FASTA format file.
     */
    boost::filesystem::path transcriptFile_;
    /**
     * Describes the expected format of the sequencing
     * fragment library.
     */
    LibraryFormat libFmt_;
    /**
     * The targets (transcripts) to be quantified.
     */
    std::vector<Transcript> transcripts_;
    /**
     * A pointer to the queue from which the fragments
     * will be read.
     */
    //std::unique_ptr<t_pool, std::function<void(t_pool*)>> threadPool_;
    std::unique_ptr<BAMQueue<FragT>> bq;

    SequenceBiasModel seqBiasModel_;

    /**
     * The cluster forest maintains the dynamic relationship
     * defined by transcripts and reads --- if two transcripts
     * share an ambiguously mapped read, then they are placed
     * in the same cluster.
     */
    std::unique_ptr<ClusterForest> clusters_;

    /**
      * The emperical fragment start position distribution
      */
    std::vector<FragmentStartPositionDistribution> fragStartDists_;

    /**
     * The emperical fragment length distribution.
     *
     */
    std::unique_ptr<FragmentLengthDistribution> flDist_;
    /**
      * The emperical error model
      */
    std::unique_ptr<AlignmentModel> alnMod_;

    /** Keeps track of the number of passes that have been
     *  made through the alignment file.
     */
    size_t quantificationPasses_;
    SpinLock sl_;
    EquivalenceClassBuilder eqBuilder_;

    /** GC-fragment bias things **/
    // One bin for each percentage GC content
    double gcFracFwd_;
    std::vector<double> observedGC_;
    std::vector<double> expectedGC_;

    // Since multiple threads can touch this dist, we
    // need atomic counters.
    ReadKmerDist<6, std::atomic<uint32_t>> readBias_;
    std::vector<double> expectedBias_;
};

#endif // ALIGNMENT_LIBRARY_HPP
