#ifndef BAMSTATS_H
#define BAMSTATS_H

#include <limits>

#include <boost/unordered_map.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/progress.hpp>

#include <htslib/sam.h>
#include <htslib/faidx.h>

#include "util.h"

namespace bamstats
{

  struct Interval {
    int32_t start;
    int32_t end;
    
    Interval(int32_t s, int32_t e) : start(s), end(e) {}
  };

  struct BaseCounts {
    typedef std::vector<uint64_t> TCoverageBp;
    typedef uint8_t TBpCovInt;
    typedef std::vector<TBpCovInt> TBpCoverage;
    
    uint32_t maxBpCov;
    uint64_t matchCount;
    uint64_t mismatchCount;
    uint64_t delCount;
    uint64_t insCount;
    uint64_t softClipCount;
    uint64_t hardClipCount;
    TCoverageBp bpWithCoverage;
    TBpCoverage cov;

    BaseCounts() : maxBpCov(std::numeric_limits<TBpCovInt>::max()), matchCount(0), mismatchCount(0), delCount(0), insCount(0), softClipCount(0), hardClipCount(0) {
      bpWithCoverage.resize(maxBpCov + 1, 0);
      cov.clear();
    }
  };

  struct ReadCounts {
    typedef uint16_t TMaxReadLength;
    typedef std::vector<TMaxReadLength> TLengthReadCount;

    int32_t maxReadLength;
    int64_t secondary;
    int64_t qcfail;
    int64_t dup;
    int64_t supplementary;
    int64_t unmap;
    int64_t mapped1;
    int64_t mapped2;
    TLengthReadCount lRc;

    ReadCounts() : maxReadLength(std::numeric_limits<TMaxReadLength>::max()), secondary(0), qcfail(0), dup(0), supplementary(0), unmap(0), mapped1(0), mapped2(0) {
      lRc.resize(maxReadLength + 1, 0);
    }
  };


  struct PairCounts {
    typedef uint16_t TMaxInsertSize;
    typedef std::vector<TMaxInsertSize> TISizePairCount;
    int32_t maxInsertSize;
    int64_t paired;
    int64_t mapped;
    int64_t mappedSameChr;
    int64_t orient[4];
    int64_t totalISizeCount;
    TISizePairCount fPlus;
    TISizePairCount rPlus;
    TISizePairCount fMinus;
    TISizePairCount rMinus;
    
    
    PairCounts() : maxInsertSize(std::numeric_limits<TMaxInsertSize>::max()), paired(0), mapped(0), mappedSameChr(0) {
      orient[0] = 0;
      orient[1] = 0;
      orient[2] = 0;
      orient[3] = 0;
      fPlus.resize(maxInsertSize + 1, 0);
      rPlus.resize(maxInsertSize + 1, 0);
      fMinus.resize(maxInsertSize + 1, 0);
      rMinus.resize(maxInsertSize + 1, 0);
    }
  };
  
  struct ReadGroupStats {
    BaseCounts bc;
    ReadCounts rc;
    PairCounts pc;
    
  ReadGroupStats() : bc(BaseCounts()), rc(ReadCounts()), pc(PairCounts()) {}
  };

  
  template<typename TConfig>
  inline int32_t
  bamStatsRun(TConfig const& c) {
    // Load bam file
    samFile* samfile = sam_open(c.bamFile.string().c_str(), "r");
    bam_hdr_t* hdr = sam_hdr_read(samfile);

    // One list of regions for every chromosome
    typedef std::vector<Interval> TChromosomeRegions;
    typedef std::vector<TChromosomeRegions> TGenomicRegions;
    TGenomicRegions gRegions;
    gRegions.resize(hdr->n_targets, TChromosomeRegions());

    // Parse regions from BED file or create one region per chromosome
    if (c.hasRegionFile) {
      // Parse regions from BED file
      std::ifstream bedFile(c.regionFile.string().c_str(), std::ifstream::in);
      if (bedFile.is_open()) {
	while (bedFile.good()) {
	  std::string line;
	  getline(bedFile, line);
	  typedef boost::tokenizer< boost::char_separator<char> > Tokenizer;
	  boost::char_separator<char> sep(" \t,;");
	  Tokenizer tokens(line, sep);
	  Tokenizer::iterator tokIter = tokens.begin();
	  std::string chrName = *tokIter++;
	  // Map chromosome names to the bam header chromosome IDs
	  int32_t chrid = bam_name2id(hdr, chrName.c_str());
	  // Valid ID?
	  if (chrid >= 0) {
	    if (tokIter!=tokens.end()) {
	      int32_t start = boost::lexical_cast<int32_t>(*tokIter++);
	      int32_t end = boost::lexical_cast<int32_t>(*tokIter++);
	      gRegions[chrid].push_back(Interval(start, end));
	    }
	  }
	}
      }
    } else {
      // Make one region for every chromosome
      for (int refIndex = 0; refIndex<hdr->n_targets; ++refIndex) gRegions[refIndex].push_back(Interval(0, hdr->target_len[refIndex]));
    }
    
    // Debug code
    //uint32_t rIndex = 0;
    //for(TGenomicRegions::const_iterator itG = gRegions.begin(); itG != gRegions.end(); ++itG, ++rIndex) {
    //for(TChromosomeRegions::const_iterator itC = itG->begin(); itC != itG->end(); ++itC) {
    //std::cout << rIndex << ',' << hdr->target_name[rIndex] << ',' << itC->start << ',' << itC->end << std::endl;
    //}
    //}

    // Read group statistics
    typedef std::set<std::string> TRgSet;
    TRgSet rgs;
    getRGs(std::string(hdr->text), rgs);
    typedef boost::unordered_map<std::string, ReadGroupStats> TRGMap;
    TRGMap rgMap;
    for(typename TRgSet::const_iterator itRg = rgs.begin(); itRg != rgs.end(); ++itRg) rgMap.insert(std::make_pair(*itRg, ReadGroupStats()));

    // Total reference base pairs
    uint64_t referencebp = 0;


    // Parse reference and BAM file
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "BAM file parsing" << std::endl;
    boost::progress_display show_progress( hdr->n_targets );

    // Parse genome
    int32_t refIndex = -1;
    char* seq = NULL;
    faidx_t* fai = fai_load(c.genome.string().c_str());
    bam1_t* rec = bam_init1();
    while (sam_read1(samfile, hdr, rec) >= 0) {
      // New chromosome?
      if ((!(rec->core.flag & BAM_FUNMAP)) && (rec->core.tid != refIndex)) {
	++show_progress;
	
	// Summarize bp-level coverage
	if (refIndex != -1) {
	  for(typename TRGMap::iterator itRg = rgMap.begin(); itRg != rgMap.end(); ++itRg) {
	    for(uint32_t i = 0; i < hdr->target_len[refIndex]; ++i) ++itRg->second.bc.bpWithCoverage[itRg->second.bc.cov[i]];
	    itRg->second.bc.cov.clear();
	  }
	  if (seq != NULL) free(seq);
	}
	refIndex = rec->core.tid;
	
	// Load chromosome
	int32_t seqlen = -1;
	std::string tname(hdr->target_name[refIndex]);
	seq = faidx_fetch_seq(fai, tname.c_str(), 0, hdr->target_len[refIndex], &seqlen);
	referencebp += hdr->target_len[refIndex];
	
	// Resize coverage vectors
	for(typename TRGMap::iterator itRg = rgMap.begin(); itRg != rgMap.end(); ++itRg) itRg->second.bc.cov.resize(hdr->target_len[refIndex], 0);
      }

      // Get the library information
      std::string rG = "DefaultLib";
      uint8_t *rgptr = bam_aux_get(rec, "RG");
      if (rgptr) {
	char* rg = (char*) (rgptr + 1);
	rG = std::string(rg);
      }
      typename TRGMap::iterator itRg = rgMap.find(rG);
      if (itRg == rgMap.end()) {
	std::cerr << "Missing read group: " << rG << std::endl;
	return 1;
      }
      
      // Paired counts
      if (rec->core.flag & BAM_FPAIRED) {
	++itRg->second.pc.paired;
	if (!((rec->core.flag & BAM_FUNMAP) || (rec->core.flag & BAM_FMUNMAP))) {
	  ++itRg->second.pc.mapped;
	  if (rec->core.tid == rec->core.mtid) ++itRg->second.pc.mappedSameChr;
	  if (rec->core.pos > rec->core.mpos) {
	    ++itRg->second.pc.totalISizeCount;
	    int32_t outerISize = rec->core.pos - rec->core.mpos + alignmentLength(rec);
	    switch(layout(rec)) {
	    case 0:
	      ++itRg->second.pc.orient[0];
	      if (outerISize < itRg->second.pc.maxInsertSize) ++itRg->second.pc.fPlus[outerISize];
	      else ++itRg->second.pc.fPlus[itRg->second.pc.maxInsertSize];
	      break;
	    case 1:
	      ++itRg->second.pc.orient[1];
	      if (outerISize < itRg->second.pc.maxInsertSize) ++itRg->second.pc.fMinus[outerISize];
	      else ++itRg->second.pc.fMinus[itRg->second.pc.maxInsertSize];
	      break;
	    case 2:
	      ++itRg->second.pc.orient[2];
	      if (outerISize < itRg->second.pc.maxInsertSize) ++itRg->second.pc.rMinus[outerISize];
	      else ++itRg->second.pc.rMinus[itRg->second.pc.maxInsertSize];
	      break;
	    case 3:
	      ++itRg->second.pc.orient[3];
	      if (outerISize < itRg->second.pc.maxInsertSize) ++itRg->second.pc.rPlus[outerISize];
	      else ++itRg->second.pc.rPlus[itRg->second.pc.maxInsertSize];
	      break;
	    default:
	      break;
	    }
	  }
	}
      }
      
      // Read counts
      if (rec->core.flag & (BAM_FSECONDARY | BAM_FQCFAIL | BAM_FDUP | BAM_FSUPPLEMENTARY | BAM_FUNMAP)) {
	if (rec->core.flag & BAM_FSECONDARY) ++itRg->second.rc.secondary;
	if (rec->core.flag & BAM_FQCFAIL) ++itRg->second.rc.qcfail;
	if (rec->core.flag & BAM_FDUP) ++itRg->second.rc.dup;
	if (rec->core.flag & BAM_FSUPPLEMENTARY) ++itRg->second.rc.supplementary;
	if (rec->core.flag & BAM_FUNMAP) ++itRg->second.rc.unmap;
	continue;
      }
      if (rec->core.flag & BAM_FREAD2) ++itRg->second.rc.mapped2;
      else ++itRg->second.rc.mapped1;
      if (rec->core.l_qseq < itRg->second.rc.maxReadLength) ++itRg->second.rc.lRc[rec->core.l_qseq];
      else ++itRg->second.rc.lRc[itRg->second.rc.maxReadLength];
      
      // Get the read sequence
      std::string sequence;
      sequence.resize(rec->core.l_qseq);
      uint8_t* seqptr = bam_get_seq(rec);
      for (int32_t i = 0; i < rec->core.l_qseq; ++i) sequence[i] = "=ACMGRSVTWYHKDBN"[bam_seqi(seqptr, i)];
      
      // Get the reference slice
      std::string refslice = boost::to_upper_copy(std::string(seq + rec->core.pos, seq + lastAlignedPosition(rec)));
      
      // Debug 
      //std::cout << matchCount << ',' << mismatchCount << ',' << delCount << ',' << insCount << ',' << softClipCount << ',' << hardClipCount << std::endl;
      //std::cout << refslice << std::endl;
      //std::cout << sequence << std::endl;
	
      uint32_t rp = 0; // reference pointer
      uint32_t sp = 0; // sequence pointer
      
      // Parse the CIGAR
      uint32_t* cigar = bam_get_cigar(rec);
      for (std::size_t i = 0; i < rec->core.n_cigar; ++i) {
	if (bam_cigar_op(cigar[i]) == BAM_CMATCH) {
	  // match or mismatch
	  for(std::size_t k = 0; k<bam_cigar_oplen(cigar[i]);++k) {
	    if (sequence[sp] == refslice[rp]) ++itRg->second.bc.matchCount;
	    else ++itRg->second.bc.mismatchCount;
	    // Count bp-level coverage
	    if (itRg->second.bc.cov[rec->core.pos + rp] < itRg->second.bc.maxBpCov) ++itRg->second.bc.cov[rec->core.pos + rp];
	    ++sp;
	    ++rp;
	  }
	} else if (bam_cigar_op(cigar[i]) == BAM_CDEL) {
	  ++itRg->second.bc.delCount;
	  rp += bam_cigar_oplen(cigar[i]);
	} else if (bam_cigar_op(cigar[i]) == BAM_CINS) {
	  ++itRg->second.bc.insCount;
	  sp += bam_cigar_oplen(cigar[i]);
	} else if (bam_cigar_op(cigar[i]) == BAM_CSOFT_CLIP) {
	  ++itRg->second.bc.softClipCount;
	  sp += bam_cigar_oplen(cigar[i]);
	} else if(bam_cigar_op(cigar[i]) == BAM_CHARD_CLIP) {
	  ++itRg->second.bc.hardClipCount;
	} else {
	  std::cerr << "Unknown Cigar options" << std::endl;
	  return 1;
	}
      }
    }
    // Summarize bp-level coverage
    if (refIndex != -1) {
      for(typename TRGMap::iterator itRg = rgMap.begin(); itRg != rgMap.end(); ++itRg) {
	for(uint32_t i = 0; i < hdr->target_len[refIndex]; ++i) ++itRg->second.bc.bpWithCoverage[itRg->second.bc.cov[i]];
	itRg->second.bc.cov.clear();
      }
      if (seq != NULL) free(seq);
    }
    
    // clean-up
    bam_destroy1(rec);
    fai_destroy(fai);
    bam_hdr_destroy(hdr);
    sam_close(samfile);
    
    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] Done." << std::endl;
    
#ifdef PROFILE
    ProfilerStop();
#endif

    // Output read counts
    std::string statFileName = c.outprefix + ".readcounts.tsv";
    std::ofstream rcfile(statFileName.c_str());
    for(typename TRGMap::iterator itRg = rgMap.begin(); itRg != rgMap.end(); ++itRg) {
      uint64_t totalReadCount = itRg->second.rc.qcfail + itRg->second.rc.dup + itRg->second.rc.unmap + itRg->second.rc.mapped1 + itRg->second.rc.mapped2;
      uint64_t mappedCount = itRg->second.rc.mapped1 + itRg->second.rc.mapped2;
      rcfile << "Sample\tLibrary\t#QCFail\tQCFailFraction\t#DuplicateMarked\tDuplicateFraction\t#Unmapped\tUnmappedFraction\t#Mapped\tMappedFraction\t#MappedRead1\t#MappedRead2\tRatioMapped2vsMapped1\t#SecondaryAlignments\tSecondaryAlignmentFraction\t#SupplementaryAlignments\tSupplementaryAlignmentFraction" << std::endl;
      rcfile << c.sampleName << "\t" << itRg->first << "\t" << itRg->second.rc.qcfail << "\t" << (double) itRg->second.rc.qcfail / (double) totalReadCount << "\t" << itRg->second.rc.dup << "\t" << (double) itRg->second.rc.dup / (double) totalReadCount << "\t" << itRg->second.rc.unmap << "\t" << (double) itRg->second.rc.unmap / (double) totalReadCount << "\t" << mappedCount << "\t" << (double) mappedCount / (double) totalReadCount << "\t" << itRg->second.rc.mapped1 << "\t" << itRg->second.rc.mapped2 << "\t" << (double) itRg->second.rc.mapped2 / (double) itRg->second.rc.mapped1 << "\t" << itRg->second.rc.secondary << "\t" << (double) itRg->second.rc.secondary / (double) mappedCount << "\t" << itRg->second.rc.supplementary << "\t" << (double) itRg->second.rc.supplementary / (double) mappedCount  << std::endl;
    }
    rcfile.close();

    // Output read length histogram
    statFileName = c.outprefix + ".readlength.tsv";
    std::ofstream rlfile(statFileName.c_str());
    for(typename TRGMap::iterator itRg = rgMap.begin(); itRg != rgMap.end(); ++itRg) {
      rlfile << "Sample\tReadlength\tCount\tLibrary" << std::endl;
      for(uint32_t i = 0; i < itRg->second.rc.lRc.size(); ++i) rlfile << c.sampleName << "\t" << i << "\t" << itRg->second.rc.lRc[i] << "\t" << itRg->first << std::endl;
    }
    rlfile.close();

    // Output paired counts
    statFileName = c.outprefix + ".pairedcounts.tsv";
    std::ofstream pcfile(statFileName.c_str());
    for(typename TRGMap::iterator itRg = rgMap.begin(); itRg != rgMap.end(); ++itRg) {
      int64_t paired = itRg->second.pc.paired / 2;
      int64_t mapped = itRg->second.pc.mapped / 2;
      int64_t mappedSameChr = itRg->second.pc.mappedSameChr / 2;
      int32_t deflayout = 0;
      int32_t maxcount = itRg->second.pc.orient[0];
      for(int32_t i = 1; i<4; ++i) {
	if (itRg->second.pc.orient[i] > maxcount) {
	  maxcount = itRg->second.pc.orient[i];
	  deflayout = i;
	}
      }
      pcfile << "Sample\tLibrary\t#Pairs\t#MappedPairs\tMappedFraction\t#MappedSameChr\tMappedSameChrFraction\tDefaultLibraryLayout" << std::endl;
      pcfile << c.sampleName << "\t" << itRg->first << "\t" << paired << "\t" << mapped << "\t" << (double) mapped / (double) paired << "\t" << mappedSameChr << "\t" << (double) mappedSameChr / (double) paired << "\t" << deflayout << std::endl;
    }
    pcfile.close();
    
    // Output error rates
    statFileName = c.outprefix + ".errorrates.tsv";
    std::ofstream erfile(statFileName.c_str());
    for(typename TRGMap::iterator itRg = rgMap.begin(); itRg != rgMap.end(); ++itRg) {
      uint64_t alignedbases = itRg->second.bc.matchCount + itRg->second.bc.mismatchCount;
      erfile << "Sample\tLibrary\t#ReferenceBases\t#AlignedBases\t#Coverage\t#MatchedBases\tMatchRate\t#MismatchedBases\tMismatchRate\t#DeletionsCigarD\tDeletionRate\t#InsertionsCigarI\tInsertionRate\t#SoftClippedBases\tSoftClipRate\t#HardClippedBases\tHardClipRate\tErrorRate" << std::endl;
      erfile << c.sampleName << "\t" << itRg->first << "\t" << referencebp << "\t" << alignedbases << "\t" << (double) alignedbases / (double) referencebp << "\t" << itRg->second.bc.matchCount << "\t" << (double) itRg->second.bc.matchCount / (double) alignedbases << "\t" << itRg->second.bc.mismatchCount << "\t" << (double) itRg->second.bc.mismatchCount / (double) alignedbases << "\t" << itRg->second.bc.delCount << "\t" << (double) itRg->second.bc.delCount / (double) alignedbases << "\t" << itRg->second.bc.insCount << "\t" << (double) itRg->second.bc.insCount / (double) alignedbases << "\t" << itRg->second.bc.softClipCount << "\t" << (double) itRg->second.bc.softClipCount / (double) alignedbases << "\t" << itRg->second.bc.hardClipCount << "\t" << (double) itRg->second.bc.hardClipCount / (double) alignedbases << "\t" << (double) (itRg->second.bc.mismatchCount + itRg->second.bc.delCount + itRg->second.bc.insCount + itRg->second.bc.softClipCount + itRg->second.bc.hardClipCount) / (double) alignedbases  << std::endl;
    }
    erfile.close();
    
    // Output coverage histograms
    statFileName = c.outprefix + ".coverage.tsv";
    std::ofstream cofile(statFileName.c_str());
    for(typename TRGMap::iterator itRg = rgMap.begin(); itRg != rgMap.end(); ++itRg) {
      cofile << "Sample\tCoverage\tCount\tLibrary" << std::endl;
      for(uint32_t i = 0; i < itRg->second.bc.bpWithCoverage.size(); ++i) cofile << c.sampleName << "\t" << i << "\t" << itRg->second.bc.bpWithCoverage[i] << "\t" << itRg->first << std::endl;
    }
    cofile.close();

    // Output insert size histograms
    statFileName = c.outprefix + ".isize.tsv";
    std::ofstream isfile(statFileName.c_str());
    for(typename TRGMap::iterator itRg = rgMap.begin(); itRg != rgMap.end(); ++itRg) {
      isfile << "Sample\tInsertSize\tCount\tLayout\tLibrary" << std::endl;
      for(uint32_t i = 0; i < itRg->second.pc.fPlus.size(); ++i) {
	isfile << c.sampleName << "\t" << i << "\t" << itRg->second.pc.fPlus[i] << "\tF+\t" << itRg->first << std::endl;
	isfile << c.sampleName << "\t" << i << "\t" << itRg->second.pc.fMinus[i] << "\tF-\t" << itRg->first << std::endl;
	isfile << c.sampleName << "\t" << i << "\t" << itRg->second.pc.rPlus[i] << "\tR+\t" << itRg->first << std::endl;
	isfile << c.sampleName << "\t" << i << "\t" << itRg->second.pc.rMinus[i] << "\tR-\t" << itRg->first << std::endl;
      }
    }
    isfile.close();
    
    return 0;
  }

}

#endif