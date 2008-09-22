#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <cassert>
#include <seqan/find.h>
#include <getopt.h>
#include <vector>
#include "alphabet.h"
#include "assert_helpers.h"
#include "endian_swap.h"
#include "ebwt.h"
#include "formats.h"
#include "sequence_io.h"
#include "tokenize.h"
#include "hit.h"
#include "pat.h"
#include "bitset.h"
#include "threading.h"

using namespace std;
using namespace seqan;

static int verbose				= 0; // be talkative
static int sanityCheck			= 0;  // enable expensive sanity checks
static int format				= FASTQ; // default read format is FASTQ
static string origString		= ""; // reference text, or filename(s)
static int revcomp				= 1; // search for reverse complements?
static int seed					= 0; // srandom() seed
static int timing				= 0; // whether to report basic timing data
static bool oneHit				= true;  // for multihits, report just one
static bool arrowMode			= false; // report SA arrows instead of locs
static int showVersion			= 0; // just print version and quit?
static int ipause				= 0; // pause before maching?
static uint32_t qUpto			= 0xffffffff; // max # of queries to read
static int skipSearch			= 0; // abort before searching
static int qSameLen				= 0; // abort before searching
static int trim5				= 0; // amount to trim from 5' end
static int trim3				= 0; // amount to trim from 3' end
static int printStats			= 0; // whether to print statistics
static int reportOpps			= 0; // whether to report # of other mappings
static int offRate				= -1; // keep default offRate
static int mismatches			= 0; // allow 0 mismatches by default
static char *patDumpfile		= NULL; // filename to dump patterns to
static bool solexa_quals		= false; //quality strings are solexa qualities, instead of phred
static int maqLike				= 1; // do maq-like searching
static int seedLen              = 28; // seed length (changed in Maq 0.6.4 from 24)
static int seedMms              = 2;  // # mismatches allowed in seed (maq's -n)
static int qualThresh           = 7;  // max qual-weighted hamming dist (maq's -e)
static int maxBts               = 100; // max # backtracks allowed in half-and-half mode
static int maxNs                = 999999; // max # Ns allowed in read
static int nsPolicy             = NS_TO_NS; // policy for handling no-confidence bases
static int nthreads             = 1;
static output_types outType		= FULL; // report hits in id+/-:<x,y,z> format
static bool randReadsNoSync     = false;
static int numRandomReads       = 2000000;
static int lenRandomReads       = 35;

static const char *short_options = "fqbh?cu:rv:sat3:5:o:e:n:l:w:p:";

#define ARG_ORIG                256
#define ARG_SEED                257
#define ARG_DUMP_PATS           258
#define ARG_ARROW               259
#define ARG_CONCISE             260
#define ARG_SOLEXA_QUALS        261
#define ARG_MAXBTS              262
#define ARG_VERBOSE             263
#define ARG_MAXNS               264
#define ARG_RANDOM_READS        265
#define ARG_RANDOM_READS_NOSYNC 266
#define ARG_NOOUT               267

static struct option long_options[] = {
	{"verbose",      no_argument,       0,            ARG_VERBOSE},
	{"sanity",       no_argument,       0,            's'},
	{"exact",        no_argument,       0,            '0'},
	{"1mm",          no_argument,       0,            '1'},
	{"2mm",          no_argument,       0,            '2'},
	{"pause",        no_argument,       &ipause,      1},
	{"orig",         required_argument, 0,            ARG_ORIG},
	{"allhits",      no_argument,       0,            'a'},
	{"concise",      no_argument,       0,            ARG_CONCISE},
	{"noout",        no_argument,       0,            ARG_NOOUT},
	{"solexa-quals", no_argument,       0,            ARG_SOLEXA_QUALS},
	{"time",         no_argument,       0,            't'},
	{"trim3",        required_argument, 0,            '3'},
	{"trim5",        required_argument, 0,            '5'},
	{"seed",         required_argument, 0,            ARG_SEED},
	{"qupto",        required_argument, 0,            'u'},
	{"offrate",      required_argument, 0,            'o'},
	{"skipsearch",   no_argument,       &skipSearch,  1},
	{"qsamelen",     no_argument,       &qSameLen,    1},
	{"stats",        no_argument,       &printStats,  1},
	{"reportopps",   no_argument,       &reportOpps,  1},
	{"version",      no_argument,       &showVersion, 1},
	{"maq",          no_argument,       &maqLike,     1},
	{"ntoa",         no_argument,       &nsPolicy,    NS_TO_AS},
	{"dumppats",     required_argument, 0,            ARG_DUMP_PATS},
	{"revcomp",      no_argument,       0,            'r'},
	{"maqerr",       required_argument, 0,            'e'},
	{"seedlen",      required_argument, 0,            'l'},
	{"seedmms",      required_argument, 0,            'n'},
	{"help",         no_argument,       0,            'h'},
	{"threads",      required_argument, 0,            'p'},
	{"arrows",       no_argument,       0,            ARG_ARROW},
	{"maxbts",       required_argument, 0,            ARG_MAXBTS},
	{"maxns",        required_argument, 0,            ARG_MAXNS},
	{"randread",     no_argument,       0,            ARG_RANDOM_READS},
	{"randreadnosync", no_argument,     0,            ARG_RANDOM_READS_NOSYNC},
	{0, 0, 0, 0} // terminator
};

/**
 * Print a detailed usage message to the provided output stream.
 */
static void printUsage(ostream& out) {
	out << "Usage: bowtie [options]* <ebwt_base> <query_in> [<hit_outfile>]" << endl
	    << "  <ebwt_base>        ebwt filename minus trailing .1.ebwt/.2.ebwt" << endl
	    << "  <query_in>         comma-separated list of files containing query reads" << endl
	    << "                     (or the sequences themselves, if -c is specified)" << endl
	    << "  <hit_outfile>      file to write hits to (default: stdout)" << endl
	    << "Options:" << endl
	    << "  -q                 query input files are FASTQ .fq/.fastq (default)" << endl
	    << "  -f                 query input files are (multi-)FASTA .fa/.mfa" << endl
	    << "  -r                 query input files are raw one-sequence-per-line" << endl
	    //<< "  -m                 query input files are Maq .bfq" << endl
	    //<< "  -x                 query input files are Solexa _seq.txt" << endl
	    << "  -c                 query sequences given on command line (as <query_in>)" << endl
	    << "  -e/--maqerr <int>  max sum of mismatch quals (rounds like maq; default: 70)" << endl
	    << "  -l/--seedlen <int> seed length (default: 28)" << endl
	    << "  -n/--seedmms <int> max mismatches in seed (can be 0-3, default: 2)" << endl
	    << "  -v <int>           report end-to-end hits w/ <=v mismatches; ignore qualities" << endl
	    << "  -5/--trim5 <int>   trim <int> bases from 5' (left) end of reads" << endl
	    << "  -3/--trim3 <int>   trim <int> bases from 3' (right) end of reads" << endl
	    << "  -p/--threads <int> number of search threads to launch (default: 1)" << endl
	    << "  -u/--qupto <int>   stop after the first <int> reads" << endl
	    //<< "  --maq              maq-like matching (forces -r, -k 24)" << endl
	    << "  -t/--time          print wall-clock time taken by search phases" << endl
		<< "  --solexa-quals     convert FASTQ qualities from solexa-scaled to phred" << endl
		<< "  --ntoa             Ns in reads become As; default: Ns match nothing" << endl
	    //<< "  -s/--sanity        enable sanity checks (increases runtime and mem usage!)" << endl
	    //<< "  --orig <str>       specify original string (for sanity-checking)" << endl
	    //<< "  --qsamelen         die with error if queries don't all have the same length" << endl
	    //<< "  --stats            write statistics after hits" << endl
	    //<< "  --reportopps       report # of other potential mapping targets for each hit" << endl
	    //<< "  -a/--allhits       if query has >1 hit, give all hits (default: 1 random hit)" << endl
	    //<< "  --arrows           report hits as top/bottom offsets into SA" << endl
	    //<< "  --randomReads      generate random reads; ignore -q/-f/-r and <query_in>" << endl
	    << "  --concise          write hits in a concise format" << endl
	    //<< "  --maxbts <int>     maximum number of backtracks allowed (default: 100)" << endl
	    << "  --maxns <int>      skip reads w/ >n no-confidence bases (default: no limit)" << endl
	    //<< "  --dumppats <file>  dump all patterns read to a file" << endl
	    << "  -o/--offrate <int> override offrate of Ebwt; must be <= value in index" << endl
	    << "  --seed <int>       seed for random number generator" << endl
	    << "  --verbose          verbose output (for debugging)" << endl
	    << "  -h/-?/--help       print this usage message" << endl
	    << "  --version          print version information and quit" << endl
	    ;
}

/**
 * Parse an int out of optarg and enforce that it be at least 'lower';
 * if it is less than 'lower', than output the given error message and
 * exit with an error and a usage message.
 */
static int parseInt(int lower, const char *errmsg) {
	long l;
	char *endPtr= NULL;
	l = strtol(optarg, &endPtr, 10);
	if (endPtr != NULL) {
		if (l < lower) {
			cerr << errmsg << endl;
			printUsage(cerr);
			exit(1);
		}
		return (int32_t)l;
	}
	cerr << errmsg << endl;
	printUsage(cerr);
	exit(1);
	return -1;
}

/**
 * Read command-line arguments
 */
static void parseOptions(int argc, char **argv) {
    int option_index = 0;
	int next_option;
	do {
		next_option = getopt_long(argc, argv, short_options, long_options, &option_index);
		switch (next_option) {
	   		case 'f': format = FASTA; break;
	   		case 'q': format = FASTQ; break;
	   		case 'r': format = RAW; break;
	   		case 'c': format = CMDLINE; break;
	   		case ARG_RANDOM_READS: format = RANDOM; break;
	   		case ARG_RANDOM_READS_NOSYNC:
	   			format = RANDOM;
	   			randReadsNoSync = true;
	   			break;
	   		case ARG_ARROW: arrowMode = true; break;
	   		case ARG_CONCISE: outType = CONCISE; break;
	   		case ARG_NOOUT: outType = NONE; break;
			case ARG_SOLEXA_QUALS: solexa_quals = true; break;
	   		case ARG_SEED:
	   			seed = parseInt(0, "--seed arg must be at least 0");
	   			break;
	   		case 'u':
	   			qUpto = (uint32_t)parseInt(1, "-u/--qupto arg must be at least 1");
	   			break;
	   		case 'p':
	   			nthreads = parseInt(1, "-p/--threads arg must be at least 1");
	   			break;
	   		case 'v':
	   			maqLike = 0;
	   			mismatches = parseInt(0, "-v arg must be at least 0");
	   			if(mismatches > 3) {
	   				cerr << "-v arg must be at most 3" << endl;
	   				exit(1);
	   			}
	   			break;
	   		case '3':
	   			trim3 = parseInt(0, "-3/--trim3 arg must be at least 0");
	   			break;
	   		case '5':
	   			trim5 = parseInt(0, "-5/--trim5 arg must be at least 0");
	   			break;
	   		case 'o':
	   			offRate = parseInt(1, "-o/--offrate arg must be at least 1");
	   			break;
	   		case 'e':
	   			qualThresh = int(parseInt(1, "-e/--err arg must be at least 1") / 10.0 + 0.5);
	   			break;
	   		case 'n':
	   			seedMms = parseInt(0, "-n/--seedmms arg must be at least 0");
	   			break;
	   		case 'l':
	   			seedLen = parseInt(20, "-l/--seedlen arg must be at least 20");
	   			break;
	   		case 'h':
	   		case '?':
				printUsage(cerr);
				exit(0);
				break;
	   		case ARG_MAXNS:
	   			maxNs = parseInt(0, "--maxns arg must be at least 0");
	   			break;
	   		case 'a': oneHit = false; break;
	   		case ARG_VERBOSE: verbose = true; break;
	   		case 's': sanityCheck = true; break;
	   		case 't': timing = true; break;
			case ARG_MAXBTS:
				if (optarg != NULL)
					maxBts = parseInt(1, "--maxbts must be at least 1");
				break;
	   		case ARG_DUMP_PATS:
	   			patDumpfile = optarg;
	   			break;
	   		case ARG_ORIG:
   				if(optarg == NULL || strlen(optarg) == 0) {
   					cerr << "--orig arg must be followed by a string" << endl;
   					printUsage(cerr);
   					exit(1);
   				}
   				origString = optarg;
	   			break;

			case -1: break; /* Done with options. */
			case 0:
				if (long_options[option_index].flag != 0)
					break;
			default:
				cerr << "Unknown option: " << (char)next_option << endl;
				printUsage(cerr);
				exit(1);
		}
	} while(next_option != -1);
	if(maqLike) {
		revcomp = true;
	}
	if(maqLike && !oneHit) {
		// No support for -a in Maq mode (yet)
		cerr << "Cannot combine -a/--allhits with Maq-like (default) mode"
		     << endl
		     << "Either omit -a/--allhits or also specify -0, -1, or -2 for end-to-end mode"
		     << endl;
		exit(1);
	}
	if(!maqLike) {
		maxBts = 999999;
	}
}

static char *argv0 = NULL;

static void sanityCheckExact(
		vector<String<Dna5> >& os,
        HitSinkPerThread& sink,
        String<Dna5>& pat,
        uint32_t patid)
{
	vector<Hit>& results = sink.retainedHits();
	vector<U32Pair> results2;
	results2.reserve(256);
	for(unsigned int i = 0; i < os.size(); i++) {
		// Forward
		Finder<String<Dna5> > finder(os[i]);
		Pattern<String<Dna5>, Horspool> pattern(pat);
		while (find(finder, pattern)) {
			results2.push_back(make_pair(i, position(finder)));
		}
	}
	sort(results.begin(), results.end());
	if(oneHit) { // isn't this guard redundant?
		assert_leq(results.size(), results2.size());
		for(int i = 0; i < (int)results.size(); i++) {
			bool foundMatch = false;
			for(int j = i; j < (int)results2.size(); j++) {
				if(results[i].h.first == results2[j].first &&
				   results[i].h.second == results2[j].second)
				{
					foundMatch = true;
					break;
				}
			}
			assert(foundMatch);
		}
	} else {
		assert_eq(results.size(), results2.size());
		for(int i = 0; i < (int)results.size(); i++) {
			assert_eq(results[i].h.first, results2[i].first);
			assert_eq(results[i].h.second, results2[i].second);
		}
	}
	if(verbose) {
		cout << "Passed orig/result sanity-check ("
			 << results2.size() << " results checked) for pattern "
			 << patid << endl;
	}
	sink.clearRetainedHits();
}

/// Macro for getting the next read, possibly aborting depending on
/// whether the result is empty or the patid exceeds the limit, and
/// marshaling the read into convenient variables.
#define GET_READ(p) \
	p->nextRead(); \
	if(p->empty() || p->patid() >= qUpto) { /* cout << "done" << endl; */ break; } \
	params.setPatId(p->patid()); \
	assert(!empty(p->patFw())); \
	String<Dna5>& patFw  = p->patFw();  \
	String<Dna5>& patRc  = p->patRc();  \
	String<char>& qualFw = p->qualFw(); \
	String<char>& qualRc = p->qualRc(); \
	String<char>& name   = p->name(); \
	uint32_t      patid  = p->patid(); \
	/* cout << name << ": " << patFw << ":" << qualFw << endl; */ \
	if(lastLen == 0) lastLen = length(patFw); \
	if(qSameLen && length(patFw) != lastLen) { \
		throw runtime_error("All reads must be the same length"); \
	}

/// Macro for getting the forward oriented version of next read,
/// possibly aborting depending on whether the result is empty or the
/// patid exceeds the limit, and marshaling the read into convenient
/// variables.
#define GET_READ_FW(p) \
	p->nextRead(); \
	if(p->empty() || p->patid() >= qUpto) break; \
	params.setPatId(p->patid()); \
	assert(!empty(p->patFw())); \
	String<Dna5>& patFw  = p->patFw();  \
	String<char>& qualFw = p->qualFw(); \
	String<char>& name   = p->name(); \
	uint32_t      patid  = p->patid(); \
	if(lastLen == 0) lastLen = length(patFw); \
	if(qSameLen && length(patFw) != lastLen) { \
		throw runtime_error("All reads must be the same length"); \
	}

/**
 * Search through a single (forward) Ebwt index for exact end-to-end
 * hits.  Assumes that index is already loaded into memory.
 */
static PatternSource*                 exactSearch_patsrc;
static HitSink*                       exactSearch_sink;
static EbwtSearchStats<String<Dna> >* exactSearch_stats;
static Ebwt<String<Dna> >*            exactSearch_ebwt;
static vector<String<Dna5> >*         exactSearch_os;
static void *exactSearchWorker(void *vp) {
	Timer *_t = new Timer(cout, "  Thread time: ", timing);
	PatternSource& _patsrc               = *exactSearch_patsrc;
	HitSink& _sink                       = *exactSearch_sink;
	EbwtSearchStats<String<Dna> >& stats = *exactSearch_stats;
	Ebwt<String<Dna> >& ebwt             = *exactSearch_ebwt;
	vector<String<Dna5> >& os            = *exactSearch_os;

	// Global initialization
	bool sanity = sanityCheck && !os.empty();
	// Per-thread initialization
	uint64_t lastHits = 0llu;
	uint32_t lastLen = 0;
	PatternSourcePerThread *patsrc;
	if(randReadsNoSync) {
		patsrc = new RandomPatternSourcePerThread(numRandomReads, lenRandomReads, nthreads, (int)(long)vp, false);
	} else {
		patsrc = new WrappedPatternSourcePerThread(_patsrc);
	}
	HitSinkPerThread sink(_sink, sanity);
	EbwtSearchParams<String<Dna> > params(
			sink,       // HitSink
	        stats,      // EbwtSearchStats
	        // Policy for how to resolve multiple hits
	        (oneHit? MHP_PICK_1_RANDOM : MHP_CHASE_ALL),
	        os,         // reference sequences
	        revcomp,    // forward AND reverse complement?
	        true,       // read is forward
	        true,       // index is forward
	        arrowMode); // arrow mode
	EbwtSearchState<String<Dna> > s(ebwt, params, seed);
    while(true) {
    	GET_READ(patsrc);
    	if(patid >= qUpto) break;
    	params.setPatId(patid);
    	patid++;

    	if(lastLen == 0) lastLen = length(patFw);
    	if(qSameLen && length(patFw) != lastLen) {
    		throw runtime_error("All reads must be the same length");
    	}
    	// Process forward-oriented read
    	s.newQuery(&patFw, &name, &qualFw);
	    ebwt.search(s, params);
	    // Optionally sanity-check the result
	    if(sanity && !oneHit && !arrowMode) {
	    	sanityCheckExact(os, sink, patFw, patid);
	    }
	    // If the forward direction matched exactly, ignore the
	    // reverse complement
	    if(sink.numHits() > lastHits) {
	    	lastHits = sink.numHits();
	    	if(oneHit) continue;
	    }
	    if(!revcomp) continue;
	    // Process reverse-complement read
		params.setFw(false);
    	s.newQuery(&patRc, &name, &qualRc);
	    ebwt.search(s, params);
	    if(sanity && !oneHit && !arrowMode) {
	    	sanityCheckExact(os, sink, patRc, patid);
	    }
	    lastHits = sink.numHits();
		params.setFw(true);
    }
    delete _t;
#ifdef BOWTIE_PTHREADS
    if((long)vp != 0L) {
    	pthread_exit(NULL);
    }
#endif
    return NULL;
}

/**
 * Search through a single (forward) Ebwt index for exact end-to-end
 * hits.  Assumes that index is already loaded into memory.
 */
static void exactSearch(PatternSource& _patsrc,
                        HitSink& _sink,
                        EbwtSearchStats<String<Dna> >& stats,
                        Ebwt<String<Dna> >& ebwt,
                        vector<String<Dna5> >& os)
{
	exactSearch_patsrc = &_patsrc;
	exactSearch_sink   = &_sink;
	exactSearch_stats  = &stats;
	exactSearch_ebwt   = &ebwt;
	exactSearch_os     = &os;
#ifdef BOWTIE_PTHREADS
	pthread_attr_t pthread_custom_attr;
	pthread_attr_init(&pthread_custom_attr);
	pthread_attr_setdetachstate(&pthread_custom_attr, PTHREAD_CREATE_JOINABLE);
	pthread_t *threads = new pthread_t[nthreads-1];
	for(int i = 0; i < nthreads-1; i++) {
		pthread_create(&threads[i], &pthread_custom_attr, exactSearchWorker, (void *)(long)(i+1));
	}
#endif
	exactSearchWorker((void*)0L);
#ifdef BOWTIE_PTHREADS
	for(int i = 0; i < nthreads-1; i++) {
		pthread_join(threads[i], NULL);
	}
#endif
}

/**
 * Given a pattern, a list of reference texts, and some other state,
 * find all hits for that pattern in all texts using a naive seed-
 * and-extend algorithm where seeds are found using Horspool.
 */
static bool findSanityHits(const String<Dna5>& pat,
                           uint32_t patid,
                           bool fw,
                           vector<String<Dna5> >& os,
                           vector<Hit>& sanityHits,
                           bool allowExact,
                           bool transpose)
{
	bool ebwtFw = !transpose;
	bool fivePrimeOnLeft = (ebwtFw == fw);
    uint32_t plen = length(pat);
	String<Dna5> half;
	reserve(half, plen);
	uint32_t bump = 0;
	if(!transpose) bump = 1;
	// Grab the unrevisitable region of pat
	for(size_t i = ((plen+bump)>>1); i < plen; i++) {
		appendValue(half, (Dna5)pat[i]);
	}
    uint32_t hlen = length(half); // length of seed (right) half
    assert_leq(hlen, plen);
    uint32_t ohlen = plen - hlen; // length of other (left) half
    assert_leq(ohlen, plen);
	Pattern<String<Dna5>, Horspool> pattern(half);
	for(size_t i = 0; i < os.size(); i++) {
		String<Dna5> o = os[i];
		if(transpose) {
			for(size_t j = 0; j < length(o)>>1; j++) {
				Dna5 tmp = o[j];
				o[j] = o[length(o)-j-1];
				o[length(o)-j-1] = tmp;
			}
		}
		Finder<String<Dna5> > finder(o);
		while (find(finder, pattern)) {
			uint32_t pos = position(finder);
			// Check the anchor to see if any characters in the
			// reference half of the alignment are Ns
			bool reject = false;
			for(size_t j = 0; j < length(half); j++) {
				if((int)o[j + pos] == 4) {
					// Reject!
					reject = true;
				}
			}
			if(reject) continue;
			FixedBitset<max_read_bp> diffs;
			if(pos >= ohlen) {
				// Extend toward the left end of the pattern, counting
				// mismatches
				for(uint32_t j = 0; j < ohlen && diffs.count() <= 1; j++) {
					if((int)o[pos-j-1] == 4) {
						// Reject!
						reject = true;
						break;
					}
					if((int)o[pos-j-1] != (int)pat[ohlen-j-1]) {
						uint32_t off = ohlen-j-1;
						if(fivePrimeOnLeft) {
							diffs.set(off);
						} else {
							// The 3' end is on on the left end of the
							// pattern, but the diffs vector should
							// encode mismatches w/r/t the 5' end, so
							// we flip
							diffs.set(plen-off-1);
						}
					}
				}
				if(reject) continue;
			}
			// If the extend yielded 1 or fewer mismatches, keep it
			if((diffs.count() == 0 && allowExact) || diffs.count() == 1) {
				uint32_t off = pos - ohlen;
				if(transpose) {
					off = length(o) - off;
					off -= length(pat);
				}
				// A hit followed by a transpose can sometimes fall
				// off the beginning of the text
				if(off < (0xffffffff - length(pat))) {
					Hit h(make_pair(i, off),
						  patid,
						  "",
						  pat,
						  "" /*no need for qualities*/,
						  fw,
						  diffs);
					sanityHits.push_back(h);
				}
			}
		}
	}
	return true;
}

/**
 * Assert that the sanityHits array has been exhausted, presumably
 * after having been reconciled against actual hits with
 * reconcileHits().  Only used in allHits mode.
 */
static bool checkSanityExhausted(const String<Dna5>& pat,
                                 uint32_t patid,
                                 bool fw,
                                 vector<Hit>& sanityHits,
                                 bool transpose)
{
    // If caller specified mustExhaust, then we additionally check
    // whether every sanityHit has now been matched up with some Ebwt
    // hit.  If not, that means that Ebwt may have missed a hit, so
    // we assert.
    size_t unfoundHits = 0;
	for(size_t j = 0; j < sanityHits.size(); j++) {
		uint32_t patid = sanityHits[j].patId;
		bool fw = sanityHits[j].fw;
		cout << "Did not find sanity hit: "
		     << (patid>>revcomp) << (fw? "+":"-")
		     << ":<" << sanityHits[j].h.first << ","
		     << sanityHits[j].h.second << ","
		     << sanityHits[j].mms.str() << ">" << endl;
		cout << "  transpose: " << transpose << endl;
		unfoundHits++;
	}
	assert_eq(0, unfoundHits); // Ebwt missed a true hit?
	return true;
}

/**
 * Assert that every hit in the hits array also occurs in the
 * sanityHits array.
 */
static bool reconcileHits(const String<Dna5>& pat,
                          uint32_t patid,
                          bool fw,
                          vector<Hit>& hits,
                          vector<Hit>& sanityHits,
                          bool allowExact,
                          bool transpose)
{
    // Sanity-check each result by checking whether it occurs
	// in the sanityHits array-of-vectors
    for(size_t i = 0; i < hits.size(); i++) {
    	const Hit& h = hits[i];
    	vector<Hit>::iterator itr;
    	bool found = false;
    	// Scan through the sanityHits vector corresponding to
    	// this hit text
    	for(itr = sanityHits.begin(); itr != sanityHits.end(); itr++) {
    		// If offset into hit text matches
			assert_gt(sanityHits.size(), 0);
    		if(h.h.first == itr->h.first && h.h.second == itr->h.second) {
    			// Assert that number of mismatches matches
    			if(h.fw != itr->fw || h.mms != itr->mms) {
    				cout << endl;
    				cout << "actual hit: fw=" << h.fw << endl;
    				cout << "sanity hit: fw=" << itr->fw << endl;
    			}
    			assert_eq(h.fw, itr->fw);
    			assert(h.mms == itr->mms);
    			found = true;
    			sanityHits.erase(itr); // Retire this sanity hit
    			break;
    		}
    	}
    	// Assert that the Ebwt hit was covered by a sanity-check hit
    	if(!found) {
    		cout << "Bowtie hit not found among " << sanityHits.size() << " sanity-check hits:" << endl
    		     << "  " << pat << endl;
    		cout << "  ";
    		cout << endl;
    		cout << patid << (fw? "+":"-") << ":<"
    		     << h.h.first << "," << h.h.second << "," << h.mms.count() << ">" << endl;
    		cout << "transpose: " << transpose << endl;
    		cout << "Candidates:" << endl;
        	for(itr = sanityHits.begin(); itr != sanityHits.end(); itr++) {
        		cout << "  " << itr->h.first << " (" << itr->h.second << ")" << endl;
        	}
    	}
    	assert(found);
    }
    return true;
}

/**
 * Assert that every hit in the hits array also occurs in the
 * sanityHits array.
 */
static void sanityCheckHits(
		const String<Dna5>& pat,
		HitSinkPerThread& sink,
        uint32_t patid,
        bool fw,
        vector<String<Dna5> >& os,
        bool allowExact,
        bool transpose)
{
	vector<Hit> sanityHits;
    vector<Hit>& hits = sink.retainedHits();
    // Accumulate hits found using a naive seed-and-extend into
    // sanityHits
	findSanityHits(pat, patid, fw, os, sanityHits, allowExact, transpose);
	if(hits.size() > 0) {
		// We hit, check that oracle also got our hits
	    assert(!oneHit || hits.size() == 1);
		if(oneHit && hits[0].mms.count() > 0) {
			// If our oneHit hit is inexact, then there had
			// better be no exact sanity hits
			for(size_t i = 0; i < sanityHits.size(); i++) {
				assert_gt(sanityHits[i].mms.count(), 0);
			}
		}
		reconcileHits(pat, patid, fw, hits, sanityHits, allowExact, transpose);
	} else if(allowExact) {
		// If we tried exact and inexact and didn't hit, then
		// oracle shouldn't have hit
		assert_eq(0, sanityHits.size());
	} else {
		// If we tried exact only and didn't hit, then oracle
		// shouldn't have any exact
		for(size_t i = 0; i < sanityHits.size(); i++) {
			assert_gt(sanityHits[i].mms.count(), 0);
		}
	}
	if(oneHit) {
		// Ignore the rest of the oracle hits
		sanityHits.clear();
	} else {
		// If in allHit mode, check that we covered *all* the
		// hits produced by the oracle
		checkSanityExhausted(pat, patid, fw, sanityHits, transpose);
	}
	assert_eq(0, sanityHits.size());
    // Check that orientation of hits squares with orientation
    // of the pattern searched
    for(size_t i = 0; i < hits.size(); i++) {
    	assert_eq(fw, hits[i].fw);
    }
    sink.clearRetainedHits();
}

/**
 * Search through a pair of Ebwt indexes, one for the forward direction
 * and one for the backward direction, for exact end-to-end hits and 1-
 * mismatch end-to-end hits.  In my experience, this is slightly faster
 * than Maq (default) mode with the -n 1 option.
 *
 * Forward Ebwt (ebwtFw) is already loaded into memory and backward
 * Ebwt (ebwtBw) is not loaded into memory.
 */
static PatternSource*                 mismatchSearch_patsrc;
static HitSink*                       mismatchSearch_sink;
static EbwtSearchStats<String<Dna> >* mismatchSearch_stats;
static Ebwt<String<Dna> >*            mismatchSearch_ebwtFw;
static Ebwt<String<Dna> >*            mismatchSearch_ebwtBw;
static vector<String<Dna5> >*         mismatchSearch_os;
static SyncBitset*                    mismatchSearch_doneMask;

static void* mismatchSearchWorkerPhase1(void *vp){
	PatternSource&         _patsrc       = *mismatchSearch_patsrc;
	HitSink&               _sink         = *mismatchSearch_sink;
	EbwtSearchStats<String<Dna> >& stats = *mismatchSearch_stats;
	Ebwt<String<Dna> >&    ebwtFw        = *mismatchSearch_ebwtFw;
	vector<String<Dna5> >& os            = *mismatchSearch_os;
	SyncBitset&            doneMask      = *mismatchSearch_doneMask;

    // Per-thread initialization
    bool sanity = sanityCheck && !os.empty() && !arrowMode;
	uint64_t lastHits = 0llu;
	uint32_t lastLen = 0; // for checking if all reads have same length
	PatternSourcePerThread *patsrc;
	if(randReadsNoSync) {
		patsrc = new RandomPatternSourcePerThread(numRandomReads, lenRandomReads, nthreads, (int)(long)vp, false);
	} else {
		patsrc = new WrappedPatternSourcePerThread(_patsrc);
	}
    HitSinkPerThread sink(_sink, sanity);
	EbwtSearchParams<String<Dna> > params(
			sink,       // HitSinkPerThread
	        stats,      // EbwtSearchStats
	        // Policy for how to resolve multiple hits
	        (oneHit? MHP_PICK_1_RANDOM : MHP_CHASE_ALL),
	        os,         // reference sequences
	        revcomp,    // forward AND reverse complement?
	        false,      // read is forward
	        true,       // index is forward
	        arrowMode); // arrow mode
	// Phase 1
	EbwtSearchState<String<Dna> > s(ebwtFw, params, seed);
	while(true) {
		GET_READ(patsrc);
		assert_eq(0, sink.retainedHits().size());
		assert_eq(lastHits, sink.numHits());
		uint32_t plen = length(patFw);
		if(plen < 2) {
			cerr << "Error: Reads must be at least 2 characters long in 1-mismatch mode" << endl;
			exit(1);
		}
		// Create state for a search in the forward index
		s.newQuery(&patRc, &name, &qualRc);
		ebwtFw.search1MismatchOrBetter(s, params,
									   true,  // allow exact hits,
									   true); // inexact hits provisional
		bool hit = sink.numHits() > lastHits;
		// Set a bit indicating this pattern is done and needn't be
		// considered by the 1-mismatch loop
		if(sanity) sanityCheckHits(patRc, sink, patid, false, os, true, false);
		assert_eq(0, sink.retainedHits().size());
		if(hit) lastHits = sink.numHits();
		if(oneHit && hit) {
			assert_eq(0, sink.numProvisionalHits());
			doneMask.set(patid);
			continue;
		}
		params.setFw(true);
		s.newQuery(&patFw, &name, &qualFw);
		if(sink.numProvisionalHits() > 0) {
			// There is a provisional inexact match for the
			// reverse-complement read, so just try exact on the
			// forward-oriented read
			ebwtFw.search(s, params);
			if(sink.numHits() > lastHits) {
				// Got one or more exact hits from the reverse
				// complement; reject provisional hits
				sink.rejectProvisionalHits();
				if(sanity) sanityCheckHits(patFw, sink, patid, true, os, true, false);
			} else {
				// No exact hits from reverse complement; accept
				// provisional hits and finish with this read
				sink.acceptProvisionalHits();
				assert_gt(sink.numHits(), lastHits);
			}
			assert_eq(0, sink.numProvisionalHits());
			if(sink.numHits() > lastHits) {
				lastHits = sink.numHits();
				if(oneHit) {
					// Update doneMask
					doneMask.set(patid);
				}
			}
			assert_eq(0, sink.retainedHits().size());
		} else {
			// There is no provisional inexact match for the
			// reverse-complement read, so try inexact on the
			// forward-oriented read
			ebwtFw.search1MismatchOrBetter(s, params,
										   true,   // allow exact hits
										   false); // no provisional hits
			bool hit = sink.numHits() > lastHits;
			// Set a bit indicating this pattern is done and needn't be
			// considered by the 1-mismatch loop
			if(sanity) sanityCheckHits(patFw, sink, patid, true, os, true, false);
			assert_eq(0, sink.retainedHits().size());
			if(hit) lastHits = sink.numHits();
			if(oneHit && hit) {
				// Update doneMask
				assert_eq(0, sink.numProvisionalHits());
				doneMask.set(patid);
			}
		}
		params.setFw(false);
	} // End read loop
#ifdef BOWTIE_PTHREADS
    if((long)vp != 0L) {
    	pthread_exit(NULL);
    }
#endif
    return NULL;
}

static void* mismatchSearchWorkerPhase2(void *vp){
	PatternSource&         _patsrc      = *mismatchSearch_patsrc;
	HitSink&               _sink        = *mismatchSearch_sink;
	EbwtSearchStats<String<Dna> >& stats = *mismatchSearch_stats;
	Ebwt<String<Dna> >&    ebwtBw       = *mismatchSearch_ebwtBw;
	vector<String<Dna5> >& os           = *mismatchSearch_os;
	SyncBitset&            doneMask     = *mismatchSearch_doneMask;

    // Per-thread initialization
    bool sanity = sanityCheck && !os.empty() && !arrowMode;
	uint64_t lastHits = 0llu;
	uint32_t lastLen = 0; // for checking if all reads have same length
	PatternSourcePerThread *patsrc;
	if(randReadsNoSync) {
		patsrc = new RandomPatternSourcePerThread(numRandomReads, lenRandomReads, nthreads, (int)(long)vp, true);
	} else {
		patsrc = new WrappedPatternSourcePerThread(_patsrc);
	}
    HitSinkPerThread sink(_sink, sanity);
	EbwtSearchParams<String<Dna> > params(
			sink,       // HitSinkPerThread
	        stats,      // EbwtSearchStats
	        // Policy for how to resolve multiple hits
	        (oneHit? MHP_PICK_1_RANDOM : MHP_CHASE_ALL),
	        os,         // reference sequences
	        revcomp,    // forward AND reverse complement?
	        true,       // read is forward
	        false,      // index is mirror index
	        arrowMode); // arrow mode
	// Phase 2
	EbwtSearchState<String<Dna> > s(ebwtBw, params, seed);
	while(true) {
		GET_READ(patsrc);
		if(doneMask.test(patid)) continue;
		s.newQuery(&patFw, &name, &qualFw);
		ebwtBw.search1MismatchOrBetter(s, params,
									   false,  // no exact hits
									   false); // no provisional hits
		// Check all hits against a naive oracle
		assert_eq(0, sink.numProvisionalHits());
		if(sanity) sanityCheckHits(patFw, sink, patid, true, os, false, true);
		assert_eq(0, sink.retainedHits().size());
		// If the forward direction matched with one mismatch, ignore
		// the reverse complement
		if(oneHit && revcomp && sink.numHits() > lastHits) {
			lastHits = sink.numHits();
			continue;
		}
		if(!revcomp) continue;
		params.setFw(false);
		s.newQuery(&patRc, &name, &qualRc);
		ebwtBw.search1MismatchOrBetter(s, params,
									   false,  // no exact hits
									   false); // no provisional hits
		assert_eq(0, sink.numProvisionalHits());
		if(sanity) sanityCheckHits(patRc, sink, patid, false, os, false, true);
		assert_eq(0, sink.retainedHits().size());
		params.setFw(true);
		lastHits = sink.numHits();
	} // End read loop
#ifdef BOWTIE_PTHREADS
	if((long)vp != 0L) {
    	pthread_exit(NULL);
    }
#endif
    return NULL;
}

/**
 * Search through a single (forward) Ebwt index for exact end-to-end
 * hits.  Assumes that index is already loaded into memory.
 */
static void mismatchSearch(PatternSource& _patsrc,
                           HitSink& _sink,
                           EbwtSearchStats<String<Dna> >& stats,
                           Ebwt<String<Dna> >& ebwtFw,
                           Ebwt<String<Dna> >& ebwtBw,
                           vector<String<Dna5> >& os)
{
	uint32_t numQs = ((qUpto == 0xffffffff) ? 16 * 1024 * 1024 : qUpto);
	SyncBitset doneMask(numQs,
		// Error message for if an allocation fails
		"Could not allocate enough memory for the read mask; please subdivide reads and\n"
		"run bowtie separately on each subset.\n");

	mismatchSearch_patsrc       = &_patsrc;
	mismatchSearch_sink         = &_sink;
	mismatchSearch_stats        = &stats;
	mismatchSearch_ebwtFw       = &ebwtFw;
	mismatchSearch_ebwtBw       = &ebwtBw;
	mismatchSearch_doneMask     = &doneMask;
	mismatchSearch_os           = &os;

	assert(ebwtFw.isInMemory());
	assert(!ebwtBw.isInMemory());

#ifdef BOWTIE_PTHREADS
	pthread_attr_t pthread_custom_attr;
	pthread_attr_init(&pthread_custom_attr);
	pthread_attr_setdetachstate(&pthread_custom_attr, PTHREAD_CREATE_JOINABLE);
	pthread_t *threads = new pthread_t[nthreads-1];
#endif

    _patsrc.setReverse(false); // don't reverse patterns

	// Phase 1
    {
		Timer _t(cout, "Time for 1-mismatch forward search: ", timing);
#ifdef BOWTIE_PTHREADS
		for(int i = 0; i < nthreads-1; i++) {
			pthread_create(&threads[i], &pthread_custom_attr, mismatchSearchWorkerPhase1, (void *)(long)(i+1));
		}
#endif
		mismatchSearchWorkerPhase1((void*)0L);
#ifdef BOWTIE_PTHREADS
		for(int i = 0; i < nthreads-1; i++) {
			pthread_join(threads[i], NULL);
		}
#endif
    }

	// Release most of the memory associated with the forward Ebwt
    ebwtFw.evictFromMemory();
	{
		// Load the rest of (vast majority of) the backward Ebwt into
		// memory
		Timer _t(cout, "Time loading Mirror Ebwt: ", timing);
		ebwtBw.loadIntoMemory();
	}
    _patsrc.reset();          // reset pattern source to 1st pattern
    _patsrc.setReverse(true); // reverse patterns
	// Sanity-check the restored version of the Ebwt
	if(sanityCheck && !os.empty()) {
		ebwtBw.checkOrigs(os, true);
	}

	// Phase 2
	{
		Timer _t(cout, "Time for 1-mismatch backward search: ", timing);
#ifdef BOWTIE_PTHREADS
		for(int i = 0; i < nthreads-1; i++) {
			pthread_create(&threads[i], &pthread_custom_attr, mismatchSearchWorkerPhase2, (void *)(long)(i+1));
		}
#endif
		mismatchSearchWorkerPhase2((void*)0L);
#ifdef BOWTIE_PTHREADS
		for(int i = 0; i < nthreads-1; i++) {
			pthread_join(threads[i], NULL);
		}
#endif
	}
}

#define SWITCH_TO_FW_INDEX() { \
	/* Evict the mirror index from memory if necessary */ \
	if(ebwtBw.isInMemory()) ebwtBw.evictFromMemory(); \
	assert(!ebwtBw.isInMemory()); \
	/* Load the forward index into memory if necessary */ \
	if(!ebwtFw.isInMemory()) { \
		Timer _t(cout, "Time loading forward index: ", timing); \
		ebwtFw.loadIntoMemory(); \
	} \
	assert(ebwtFw.isInMemory()); \
	_patsrc.reset(); /* rewind pattern source to first pattern */ \
	_patsrc.setReverse(false); /* tell pattern source not to reverse patterns */ \
}

#define SWITCH_TO_BW_INDEX() { \
	/* Evict the forward index from memory if necessary */ \
	if(ebwtFw.isInMemory()) ebwtFw.evictFromMemory(); \
	assert(!ebwtFw.isInMemory()); \
	/* Load the forward index into memory if necessary */ \
	if(!ebwtBw.isInMemory()) { \
		Timer _t(cout, "Time loading mirror index: ", timing); \
		ebwtBw.loadIntoMemory(); \
	} \
	assert(ebwtBw.isInMemory()); \
	_patsrc.reset(); /* rewind pattern source to first pattern */ \
	_patsrc.setReverse(true); /* tell pattern source to reverse patterns */ \
}

#define ASSERT_NO_HITS_FW(ebwtfw) \
	if(sanityCheck && os.size() > 0) { \
		vector<Hit> hits; \
		uint32_t threeRevOff = (seedMms <= 3) ? s : 0; \
		uint32_t twoRevOff   = (seedMms <= 2) ? s : 0; \
		uint32_t oneRevOff   = (seedMms <= 1) ? s : 0; \
		uint32_t unrevOff    = (seedMms == 0) ? s : 0; \
		BacktrackManager<TStr>::naiveOracle( \
		        os, \
				patFw, \
				plen, \
		        qualFw, \
		        name, \
		        patid, \
		        hits, \
		        qualCutoff, \
		        unrevOff, \
		        oneRevOff, \
		        twoRevOff, \
		        threeRevOff, \
		        true,        /* fw */ \
		        ebwtfw,      /* ebwtFw */ \
		        0,           /* iham */ \
		        NULL,        /* muts */ \
		        false,       /* halfAndHalf */ \
		        ebwtfw);     /* invert */ \
		if(hits.size() > 0) { \
			/* Print offending hit obtained by oracle */ \
			BacktrackManager<TStr>::printHit( \
				os, \
				hits[0], \
				patFw, \
				plen, \
			    unrevOff, \
			    oneRevOff, \
			    twoRevOff, \
			    threeRevOff, \
			    ebwtfw);  /* ebwtFw */ \
		} \
		assert_eq(0, hits.size()); \
	}

#define ASSERT_NO_HITS_RC(ebwtfw) \
	if(sanityCheck && os.size() > 0) { \
		vector<Hit> hits; \
		uint32_t threeRevOff = (seedMms <= 3) ? s : 0; \
		uint32_t twoRevOff   = (seedMms <= 2) ? s : 0; \
		uint32_t oneRevOff   = (seedMms <= 1) ? s : 0; \
		uint32_t unrevOff    = (seedMms == 0) ? s : 0; \
		BacktrackManager<TStr>::naiveOracle( \
		        os, \
				patRc, \
				plen, \
		        qualRc, \
		        name, \
		        patid, \
		        hits, \
		        qualCutoff, \
		        unrevOff, \
		        oneRevOff, \
		        twoRevOff, \
		        threeRevOff, \
		        false,       /* fw */ \
		        ebwtfw,      /* ebwtFw */ \
		        0,           /* iham */ \
		        NULL,        /* muts */ \
		        false,       /* halfAndHalf */ \
		        !ebwtfw);    /* invert */ \
		if(hits.size() > 0) { \
			/* Print offending hit obtained by oracle */ \
			BacktrackManager<TStr>::printHit( \
				os, \
				hits[0], \
				patRc, \
				plen, \
			    unrevOff, \
			    oneRevOff, \
			    twoRevOff, \
			    threeRevOff, \
			    ebwtfw);  /* ebwtFw */ \
		} \
		assert_eq(0, hits.size()); \
	}

template<typename TStr>
static void twoOrThreeMismatchSearch(
        PatternSource& _patsrc,         /// pattern source
        HitSink& _sink,                 /// hit sink
        EbwtSearchStats<TStr>& stats,   /// statistics (mostly unused)
        Ebwt<TStr>& ebwtFw,             /// index of original text
        Ebwt<TStr>& ebwtBw,             /// index of mirror text
        vector<String<Dna5> >& os,      /// text strings, if available (empty otherwise)
        bool two = true)                /// true -> 2, false -> 3
{
	// Global initialization
	assert(revcomp);
	assert(ebwtFw.isInMemory());
	ASSERT_ONLY(int seedMms = two ? 2 : 3);   // dummy; used in macros
	ASSERT_ONLY(int qualCutoff = 0xffffffff); // dummy; used in macros
	uint32_t numQs = ((qUpto == 0xffffffff) ? 16 * 1024 * 1024 : qUpto);
	vector<bool> doneMask(numQs, false);
	MUTEX_T doneMaskLock;
	MUTEX_INIT(doneMaskLock);
	// Per-thread initialization
	uint32_t lastLen = 0; // for checking if all reads have same length
	uint32_t numPats;
	PatternSourcePerThread *patsrc;
	if(randReadsNoSync) {
		patsrc = new RandomPatternSourcePerThread(numRandomReads, lenRandomReads, nthreads, (int)1, false);
	} else {
		patsrc = new WrappedPatternSourcePerThread(_patsrc);
	}
	HitSinkPerThread sink(_sink);
	EbwtSearchParams<TStr> params(
			sink,        // HitSink
	        stats,       // EbwtSearchStats
	        // Policy for how to resolve multiple hits
	        (oneHit? MHP_PICK_1_RANDOM : MHP_CHASE_ALL),
	        os,          // reference sequences
	        revcomp,     // forward AND reverse complement?
	        true,        // read is forward
	        true,        // index is forward
	        arrowMode);  // arrow mode (irrelevant here)
	{
		// Phase 1: Consider cases 1R and 2R
		Timer _t(cout, "End-to-end 2-mismatch Phase 1: ", timing);
		BacktrackManager<TStr> btr(
				ebwtFw, params,
		        0, 0,           // 5, 3depth
		        0,              // unrevOff
		        0,              // 1revOff
		        0,              // 2revOff
		        0,              // 3revOff
		        0, 0,           // itop, ibot
		        0xffffffff,     // qualThresh
		        maxBts,         // max backtracks
		        0,              // reportSeedlings (don't)
		        NULL,           // seedlings
		        NULL,           // mutations
		        verbose,        // verbose
		        true,           // oneHit
		        seed,           // seed
		        &os,
		        false);         // considerQuals
		EbwtSearchState<TStr> s(ebwtFw, params, seed);
	    while(true) {
			GET_READ(patsrc);
			{
				// Expand doneMask if necessary
				MUTEX_LOCK(doneMaskLock);
				if(patid >= doneMask.size()) {
					// Double size of doneMask
	    		try {
					doneMask.resize(doneMask.size()*2, false);
	    		} catch(bad_alloc& ba) {
	    			cerr << "Could not resize doneMask to new length: " << (doneMask.size()*2) << endl;
	    			cerr << "Please subdivide the read set and invoke bowtie separately for each subdivision" << endl;
	    			exit(1);
	    		}
					assert_lt(patid, doneMask.size());
				}
				MUTEX_UNLOCK(doneMaskLock);
			}
			// If requested, check that this read has the same length
			// as all the previous ones
			size_t plen = length(patFw);
			if(qSameLen) {
				if(lastLen == 0) lastLen = plen;
				else assert_eq(lastLen, plen);
			}
			if(plen < 3 && two) {
				cerr << "Error: Read (" << name << ") is less than 3 characters long" << endl;
				exit(1);
			}
			else if(plen < 4) {
				cerr << "Error: Read (" << name << ") is less than 4 characters long" << endl;
				exit(1);
			}
			// Do an exact-match search on the forward pattern, just in
			// case we can pick it off early here
			uint64_t numHits = sink.numHits();
	    	s.newQuery(&patFw, &name, &qualFw);
		    ebwtFw.search(s, params);
			if(sink.numHits() > numHits) {
				assert_eq(numHits+1, sink.numHits());
				MUTEX_LOCK(doneMaskLock);
				doneMask[patid] = true;
				MUTEX_UNLOCK(doneMaskLock);
				continue;
			}
			// Set up backtracker with reverse complement
			params.setFw(false);
			btr.setQuery(&patRc, &qualRc, &name);
			// Calculate the halves
			uint32_t s = plen;
			uint32_t s5 = (s >> 1) + (s & 1); // length of 5' half of seed
			// Set up the revisitability of the halves
			btr.setOffs(0, 0, s5, s5, two ? s : s5, s);
			ASSERT_ONLY(numHits = sink.numHits());
			bool hit = btr.backtrack();
			assert(hit  || numHits == sink.numHits());
			assert(!hit || numHits <  sink.numHits());
			if(hit) {
				MUTEX_LOCK(doneMaskLock);
				doneMask[patid] = true;
				MUTEX_UNLOCK(doneMaskLock);
			}
			params.setFw(true);
	    }
	    // Threads join at end of Phase 1
	    numPats = _patsrc.patid();
	    assert_leq(numPats, doneMask.size());
	}
	// Unload forward index and load mirror index
	SWITCH_TO_BW_INDEX();
	patsrc->reset();
	params.setEbwtFw(false);
	{
		Timer _t(cout, "End-to-end 2-mismatch Phase 2: ", timing);
		BacktrackManager<TStr> bt(
				ebwtBw, params,
		        0, 0,           // 5, 3depth
		        0,              // unrevOff
		        0,              // 1revOff
		        0,              // 2revOff
		        0,              // 3revOff
		        0, 0,           // itop, ibot
		        0xffffffff,     // qualThresh
		        maxBts,         // max backtracks
		        0,              // reportSeedlings (no)
		        NULL,           // seedlings
			    NULL,           // mutations
		        verbose,        // verbose
		        true,           // oneHit
			    seed+1,         // seed
			    &os,
			    false);         // considerQuals
		params.setFw(true);  // looking at forward strand
	    while(true) {
			GET_READ(patsrc);
			{
				MUTEX_LOCK(doneMaskLock);
		    	assert_lt(patid, doneMask.capacity());
		    	assert_lt(patid, doneMask.size());
		    	bool done = doneMask[patid];
		    	MUTEX_UNLOCK(doneMaskLock);
				if(done) continue;
	    	}
			size_t plen = length(patFw);
			bt.setQuery(&patFw, &qualFw, &name);
			// Calculate the halves
			uint32_t s = plen;
			uint32_t s3 = s >> 1; // length of 3' half of seed
			uint32_t s5 = (s >> 1) + (s & 1); // length of 5' half of seed
			// Set up the revisitability of the halves
			bt.setOffs(0, 0, s5, s5, two? s : s5, s);
			ASSERT_ONLY(uint64_t numHits = sink.numHits());
			bool hit = bt.backtrack();
			assert(hit  || numHits == sink.numHits());
			assert(!hit || numHits <  sink.numHits());
			if(hit) {
				MUTEX_LOCK(doneMaskLock);
				doneMask[patid] = true;
				MUTEX_UNLOCK(doneMaskLock);
				continue;
			}
			// Try 2 backtracks in the 3' half of the reverse complement read
			params.setFw(false);  // looking at reverse complement
			bt.setQuery(&patRc, &qualRc, &name);
			// Set up the revisitability of the halves
			bt.setOffs(0, 0, s3, s3, two? s : s3, s);
			ASSERT_ONLY(numHits = sink.numHits());
			hit = bt.backtrack();
			assert(hit  || numHits == sink.numHits());
			assert(!hit || numHits <  sink.numHits());
			if(hit) {
				MUTEX_LOCK(doneMaskLock);
				doneMask[patid] = true;
				MUTEX_UNLOCK(doneMaskLock);
			}
			params.setFw(true);  // looking at forward strand
	    }
	    // Threads join at end of Phase 2
	    assert_eq(numPats, _patsrc.patid());
	}
	SWITCH_TO_FW_INDEX();
	patsrc->reset();
	params.setEbwtFw(true);
	{
		// Phase 3: Consider cases 3R and 4R and generate seedlings for
		// case 4F
		Timer _t(cout, "End-to-end 2-mismatch Phase 3: ", timing);
		// BacktrackManager to search for seedlings for case 4F
		BacktrackManager<TStr> bt(
				ebwtFw, params,
		        0, 0,           // 3, 5depth
                0,              // unrevOff
                0,              // 1revOff
                0,              // 2revOff
                0,              // 3revOff
		        0, 0,           // itop, ibot
		        0xffffffff,     // qualThresh (none)
		        maxBts,         // max backtracks
		        0,              // reportSeedlings (don't)
		        NULL,           // seedlings
			    NULL,           // mutations
		        verbose,        // verbose
		        true,           // oneHit
			    seed+3,         // seed
			    &os,
			    false);         // considerQuals
		BacktrackManager<TStr> bthh(
				ebwtFw, params,
		        0, 0,           // 3, 5depth
		        0,              // unrevOff
		        0,              // 1revOff
		        0,              // 2revOff
		        0,              // 3revOff
		        0, 0,           // itop, ibot
		        0xffffffff,     // qualThresh
		        maxBts,         // max backtracks
		        0,              // reportSeedlings (don't)
		        NULL,           // seedlings
			    NULL,           // mutations
		        verbose,        // verbose
		        true,           // oneHit
			    seed+5,         // seed
			    &os,
			    false,          // considerQuals
			    true);          // halfAndHalf
		params.setFw(true);
	    while(true) {
			GET_READ(patsrc);
			{
				MUTEX_LOCK(doneMaskLock);
		    	assert_lt(patid, doneMask.capacity());
		    	assert_lt(patid, doneMask.size());
		    	bool done = doneMask[patid];
		    	MUTEX_UNLOCK(doneMaskLock);
				if(done) continue;
	    	}
			uint32_t plen = length(patFw);
			// Calculate the halves
			uint32_t s = plen;
			uint32_t s3 = s >> 1; // length of 3' half of seed
			uint32_t s5 = (s >> 1) + (s & 1); // length of 5' half of seed
			bt.setQuery(&patFw, &qualFw, &name);
			// Set up the revisitability of the halves
			bt.setOffs(0, 0,
			           s3,
			           s3,
			           two? s : s3,
			           s);
			ASSERT_ONLY(uint64_t numHits = sink.numHits());
			bool hit = bt.backtrack();
			assert(hit  || numHits == sink.numHits());
			assert(!hit || numHits <  sink.numHits());
			if(hit) continue;

			// Try a half-and-half on the forward read
			bool gaveUp = false;
			bthh.setQuery(&patFw, &qualFw, &name);
			// Processing the forward pattern with the forward index;
			// s3 ("lo") half is on the right
			bthh.setOffs(s3, s,
			             0,
			             two ? s3 : 0,
			             two ? s  : s3,
			             s);
			ASSERT_ONLY(numHits = sink.numHits());
			hit = bthh.backtrack();
			if(bthh.numBacktracks() == bthh.maxBacktracks()) {
				gaveUp = true;
			}
			bthh.resetNumBacktracks();
			assert(hit  || numHits == sink.numHits());
			assert(!hit || numHits <  sink.numHits());
			if(hit) continue;

#ifndef NDEBUG
			// The forward version of the read doesn't hit
	    	// at all!  Check with the oracle to make sure it agrees.
	    	if(!gaveUp) {
	    		ASSERT_NO_HITS_FW(true);
	    	}
#endif

			// Try a half-and-half on the reverse complement read
	    	gaveUp = false;
			params.setFw(false);
			bthh.setQuery(&patRc, &qualRc, &name);
			// Processing the forward pattern with the forward index;
			// s5 ("hi") half is on the right
			bthh.setOffs(s5, s,
			             0,
			             two ? s5 : 0,
			             two ? s  : s5,
			             s);
			ASSERT_ONLY(numHits = sink.numHits());
			hit = bthh.backtrack();
			if(bthh.numBacktracks() == bthh.maxBacktracks()) {
				gaveUp = true;
			}
			bthh.resetNumBacktracks();
			assert(hit  || numHits == sink.numHits());
			assert(!hit || numHits <  sink.numHits());
			params.setFw(true);
			if(hit) continue;

#ifndef NDEBUG
			// The reverse-complement version of the read doesn't hit
	    	// at all!  Check with the oracle to make sure it agrees.
	    	if(!gaveUp) {
				ASSERT_NO_HITS_RC(true);
	    	}
#endif
	    }
	    // Threads join at end of Phase 3
	    assert(numPats == _patsrc.patid() || numPats+2 == _patsrc.patid());
	}
	return;
}

/**
 * Search for a good alignments for each read using criteria that
 * correspond somewhat faithfully to Maq's.  Search is aided by a pair
 * of Ebwt indexes, one for the original references, and one for the
 * transpose of the references.  Neither index should be loaded upon
 * entry to this function.
 *
 * Like Maq, we treat the first 24 base pairs of the read (those
 * closest to the 5' end) differently from the remainder of the read.
 * We call the first 24 base pairs the "seed."
 */
template<typename TStr>
static void seededQualCutoffSearch(
		int seedLen,                    /// length of seed (not a maq option)
        int qualCutoff,                 /// maximum sum of mismatch qualities
                                        /// like maq map's -e option
                                        /// default: 70
        int seedMms,                    /// max # mismatches allowed in seed
                                        /// (like maq map's -n option)
                                        /// Can only be 1 or 2, default: 1
        PatternSource& _patsrc,         /// pattern source
        HitSink& _sink,                 /// hit sink
        EbwtSearchStats<TStr>& stats,   /// statistics (mostly unused)
        Ebwt<TStr>& ebwtFw,             /// index of original text
        Ebwt<TStr>& ebwtBw,             /// index of mirror text
        vector<String<Dna5> >& os)    /// text strings, if available (empty otherwise)
{
	// Global intialization
	assert(revcomp);
	assert_leq(seedMms, 3);
	uint32_t numQs = ((qUpto == 0xffffffff) ? 16 * 1024 * 1024 : qUpto);
	vector<bool> doneMask(numQs, false);
	MUTEX_T doneMaskLock;
	MUTEX_INIT(doneMaskLock);
	// Per-thread initialization
	uint32_t numPats;
	uint32_t lastLen = 0; // for checking if all reads have same length
	uint32_t s = seedLen;
	uint32_t s3 = s >> 1; // length of 3' half of seed
	uint32_t s5 = (s >> 1) + (s & 1); // length of 5' half of seed
	PatternSourcePerThread *patsrc;
	if(randReadsNoSync) {
		patsrc = new RandomPatternSourcePerThread(numRandomReads, lenRandomReads, nthreads, (int)(long)(0L), false);
	} else {
		patsrc = new WrappedPatternSourcePerThread(_patsrc);
	}
	HitSinkPerThread sink(_sink);
	EbwtSearchParams<TStr> params(
			sink,        // HitSink
	        stats,       // EbwtSearchStats
	        // Policy for how to resolve multiple hits
	        (oneHit? MHP_PICK_1_RANDOM : MHP_CHASE_ALL),
	        os,          // reference sequences
	        revcomp,     // forward AND reverse complement?
	        true,        // read is forward
	        true,        // index is forward
	        arrowMode);  // arrow mode (irrelevant here)
	SWITCH_TO_FW_INDEX();
	params.setEbwtFw(true);
	{
		// Phase 1: Consider cases 1R and 2R
		Timer _t(cout, "Seeded quality search Phase 1: ", timing);
		// BacktrackManager for finding exact hits for the forward-
		// oriented read
		BacktrackManager<TStr> btf(
				ebwtFw, params,
		        0, 0,                  // 5, 3depth
		        0,                     // unrevOff,
		        0,                     // 1revOff
		        0,                     // 2revOff
		        0,                     // 3revOff
		        0, 0,                  // itop, ibot
		        qualCutoff,            // qualThresh
		        maxBts,                // max backtracks
		        0,                     // reportSeedlings (don't)
		        NULL,                  // seedlings
		        NULL,                  // mutations
		        verbose,               // verbose
		        true,                  // oneHit
		        seed,                  // seed
		        &os,
		        false);                // considerQuals
		BacktrackManager<TStr> bt(
				ebwtFw, params,
		        0, 0,                  // 5, 3depth
		        (seedMms > 0)? s5 : s, // unrevOff,
		        (seedMms > 1)? s5 : s, // 1revOff
		        (seedMms > 2)? s5 : s, // 2revOff
		        (seedMms > 3)? s5 : s, // 3revOff
		        0, 0,                  // itop, ibot
		        qualCutoff,            // qualThresh
		        maxBts,                // max backtracks
		        0,                     // reportSeedlings (don't)
		        NULL,                  // seedlings
		        NULL,                  // mutations
		        verbose,               // verbose
		        true,                  // oneHit
		        seed,                  // seed
		        &os);
	    while(true) {
	    	GET_READ(patsrc);
			{
				// Expand doneMask if necessary
				MUTEX_LOCK(doneMaskLock);
				if(patid >= doneMask.size()) {
					try {
					// Double size of doneMask
					try {
					doneMask.resize(doneMask.size()*2, false);
					assert_lt(patid, doneMask.size());
	    		} catch(bad_alloc& ba) {
	    			cerr << "Could not resize doneMask to new length: " << (doneMask.size()*2) << endl;
	    			cerr << "Please subdivide the read set and invoke bowtie separately for each subdivision" << endl;
	    			exit(1);
	    		}
	    		} catch(bad_alloc& ba) {
	    			cerr << "Could not resize doneMask to new length: " << (doneMask.size()*2) << endl;
	    			cerr << "Please subdivide the read set and invoke bowtie separately for each subdivision" << endl;
	    			exit(1);
	    		}
				}
				MUTEX_UNLOCK(doneMaskLock);
			}
			size_t plen = length(patFw);
			if(qSameLen) {
				if(lastLen == 0) lastLen = plen;
				else assert_eq(lastLen, plen);
			}
			if(plen < 2 && seedMms >= 1) {
				cerr << "Error: Read (" << name << ") is less than 2 characters long" << endl;
				exit(1);
			}
			else if(plen < 3 && seedMms >= 2) {
				cerr << "Error: Read (" << name << ") is less than 3 characters long" << endl;
				exit(1);
			}
			else if(plen < 4 && seedMms >= 3) {
				cerr << "Error: Read (" << name << ") is less than 4 characters long" << endl;
				exit(1);
			}
	    	// Check and see if the distribution of Ns disqualifies
	    	// this read right off the bat
			if(nsPolicy == NS_TO_NS) {
				size_t slen = min<size_t>(plen, seedLen);
				int ns = 0;
				bool done = false;
				for(size_t i = 0; i < slen; i++) {
					if((int)(Dna5)patFw[i] == 4) {
						if(++ns > seedMms) {
							done = true;
							break;
						}
					}
				}
				if(done) {
					ASSERT_NO_HITS_FW(true);
					ASSERT_NO_HITS_RC(true);
					MUTEX_LOCK(doneMaskLock);
					doneMask[patid] = true;
					MUTEX_UNLOCK(doneMaskLock);
					continue;
				}
			}
			// Do an exact-match search on the forward pattern, just in
			// case we can pick it off early here
			uint64_t numHits = sink.numHits();
			btf.setQuery(&patFw, &qualFw, &name);
	    	btf.setOffs(0, 0, plen, plen, plen, plen);
	    	btf.backtrack();
			if(sink.numHits() > numHits) {
				assert_eq(numHits+1, sink.numHits());
				MUTEX_LOCK(doneMaskLock);
				doneMask[patid] = true;
				MUTEX_UNLOCK(doneMaskLock);
				continue;
			}
			// Set up backtracker with reverse complement
			params.setFw(false);
			uint32_t qs = min<uint32_t>(plen, s);
			// Set up special seed bounds
			if(qs < s) {
				uint32_t qs5 = (qs >> 1) + (qs & 1);
				bt.setOffs(0, 0, (seedMms > 0)? qs5 : qs,
				                 (seedMms > 1)? qs5 : qs,
				                 (seedMms > 2)? qs5 : qs,
				                 (seedMms > 3)? qs5 : qs);
			}
			bt.setQuery(&patRc, &qualRc, &name);
			ASSERT_ONLY(numHits = sink.numHits());
			bool hit = bt.backtrack();
			// Restore default seed bounds
			if(qs < s) {
				bt.setOffs(0, 0, (seedMms > 0)? s5 : s,
				                 (seedMms > 1)? s5 : s,
				                 (seedMms > 2)? s5 : s,
				                 (seedMms > 3)? s5 : s);
			}
			assert(hit  || numHits == sink.numHits());
			assert(!hit || numHits <  sink.numHits());
			if(hit) {
				// If we reach here, then we obtained a hit for case
				// 1R, 2R or 3R and can stop considering this read
				MUTEX_LOCK(doneMaskLock);
				doneMask[patid] = true;
				MUTEX_UNLOCK(doneMaskLock);
			} else {
				// If we reach here, then cases 1R, 2R, and 3R have
				// been eliminated and the read needs further
				// examination
			}
			params.setFw(true);
	    }
	    // Threads join at end of Phase 1
	    numPats = _patsrc.patid();
	}
	// Unload forward index and load mirror index
	SWITCH_TO_BW_INDEX();
	patsrc->reset();
	params.setEbwtFw(false);
	PartialAlignmentManager *pamRc = NULL;
	if(seedMms > 0) pamRc = new PartialAlignmentManager();
	{
		// Phase 2: Consider cases 1F, 2F and 3F and generate seedlings
		// for case 4R
		Timer _t(cout, "Seeded quality search Phase 2: ", timing);
		// BacktrackManager to search for hits for cases 1F, 2F, 3F
		BacktrackManager<TStr> btf(
				ebwtBw, params,
		        0, 0,                  // 5, 3depth
                (seedMms > 0)? s5 : s, // unrevOff
                (seedMms > 1)? s5 : s, // 1revOff
                (seedMms > 2)? s5 : s, // 2revOff
                (seedMms > 3)? s5 : s, // 3revOff
		        0, 0,                  // itop, ibot
		        qualCutoff,            // qualThresh
		        maxBts,                // max backtracks
		        0,                     // reportSeedlings (no)
		        NULL,                  // partial alignment manager
			    NULL,                  // mutations
		        verbose,               // verbose
		        true,                  // oneHit
			    seed+1,                // seed
			    &os);                  // reference sequences
		// BacktrackManager to search for partial alignments for case 4R
		BacktrackManager<TStr> btr(
				ebwtBw, params,
		        0, 0,                  // 5, 3depth
		        s3,                    // unrevOff
		        (seedMms > 1)? s3 : s, // 1revOff
				(seedMms > 2)? s3 : s, // 2revOff
				(seedMms > 3)? s3 : s, // 3revOff
		        0, 0,                  // itop, ibot
		        qualCutoff,            // qualThresh (none)
		        maxBts,                // max backtracks
		        seedMms,               // report partials (up to seedMms mms)
		        pamRc,                 // partial alignment manager
			    NULL,                  // mutations
		        verbose,               // verbose
		        true,                  // oneHit
			    seed+2,                // seed
			    &os);                  // reference sequences
	    while(true) {
			GET_READ(patsrc);
			{
				MUTEX_LOCK(doneMaskLock);
		    	assert_lt(patid, doneMask.capacity());
		    	assert_lt(patid, doneMask.size());
		    	bool done = doneMask[patid];
		    	MUTEX_UNLOCK(doneMaskLock);
				if(done) continue;
	    	}
			// If we reach here, then cases 1R, 2R, and 3R have been
	    	// eliminated.  The next most likely cases are 1F, 2F and
	    	// 3F...
			params.setFw(true);  // looking at forward strand
			size_t plen = length(patFw);
			btf.setQuery(&patFw, &qualFw, &name);
			uint32_t qs = min<uint32_t>(plen, s);
			// Set up special seed bounds
			if(qs < s) {
				uint32_t qs5 = (qs >> 1) + (qs & 1); // length of 5' half of seed
				btf.setOffs(0, 0,
				            (seedMms > 0)? qs5 : qs,
				            (seedMms > 1)? qs5 : qs,
				            (seedMms > 2)? qs5 : qs,
				            (seedMms > 3)? qs5 : qs);
			}
			ASSERT_ONLY(uint64_t numHits = sink.numHits());
			// Do a 12/24 backtrack on the forward-strand read using
			// the mirror index.  This will find all case 1F, 2F
			// and 3F hits.
			bool hit = btf.backtrack();
			// Restore default seed bounds
			if(qs < s) {
				btf.setOffs(0, 0,
				            (seedMms > 0)? s5 : s,
				            (seedMms > 1)? s5 : s,
				            (seedMms > 2)? s5 : s,
				            (seedMms > 3)? s5 : s);
			}
			assert(hit  || numHits == sink.numHits());
			assert(!hit || numHits <  sink.numHits());
			if(hit) {
				// The reverse complement hit, so we're done with this
				// read
				MUTEX_LOCK(doneMaskLock);
				doneMask[patid] = true;
				MUTEX_UNLOCK(doneMaskLock);
				continue;
			}
			// No need to collect partial alignments if we're not
			// allowing mismatches in the 5' seed half
			if(seedMms == 0) continue;

			// If we reach here, then cases 1F, 2F, 3F, 1R, 2R, and 3R
			// have been eliminated, leaving us with cases 4F and 4R
			// (the cases with 1 mismatch in the 5' half of the seed)
			params.setFw(false);  // looking at reverse-comp strand
			qs = min<uint32_t>(plen, s);
			// Set up special seed bounds
			if(qs < s) {
				uint32_t qs3 = qs >> 1;
				btr.setOffs(0, 0,
				            qs3,
				            (seedMms > 1)? qs3 : qs,
				            (seedMms > 2)? qs3 : qs,
				            (seedMms > 3)? qs3 : qs);
			}
			btr.setQuery(&patRc, &qualRc, &name);
			btr.setQlen(s); // just look at the seed
			// Find partial alignments for case 4R
			hit = btr.backtrack();
			// Restore default seed bounds
			if(qs < s) {
				btr.setOffs(0, 0,
				            s3,
				            (seedMms > 1)? s3 : s,
				            (seedMms > 2)? s3 : s,
				            (seedMms > 3)? s3 : s);
			}
#ifndef NDEBUG
			if(seedMms > 0) {
				vector<PartialAlignment> partials;
				assert(pamRc != NULL);
				pamRc->getPartials(patid, partials);
				if(hit) assert_gt(partials.size(), 0);
				for(size_t i = 0; i < partials.size(); i++) {
					uint32_t pos0 = partials[i].entry.pos0;
					assert_lt(pos0, s5);
					uint8_t oldChar = (uint8_t)patRc[pos0];
					assert_neq(oldChar, partials[i].entry.char0);
					if(partials[i].entry.pos1 != 0xff) {
						uint32_t pos1 = partials[i].entry.pos1;
						assert_lt(pos1, s5);
						oldChar = (uint8_t)patRc[pos1];
						assert_neq(oldChar, partials[i].entry.char1);
						if(partials[i].entry.pos2 != 0xff) {
							uint32_t pos2 = partials[i].entry.pos2;
							assert_lt(pos2, s5);
							oldChar = (uint8_t)patRc[pos2];
							assert_neq(oldChar, partials[i].entry.char2);
						}
					}
				}
			}
#endif
	    }
	    // Threads join at end of Phase 1
	}
	if(seedMms == 0) {
		// If we're not allowing any mismatches in the seed, then there
		// is no need to continue to phases 3 and 4
		assert(pamRc == NULL);
		return;
	}
	// Unload mirror index and load forward index
	SWITCH_TO_FW_INDEX();
	params.setEbwtFw(true);
	patsrc->reset();
	PartialAlignmentManager *pamFw = NULL;
	try {
		if(seedMms > 0) pamFw = new PartialAlignmentManager();
	} catch(bad_alloc& ba) {
		cerr << "Could not reserve space for PartialAlignmentManager" << endl;
		cerr << "Please subdivide the read set and invoke bowtie separately for each subdivision" << endl;
		exit(1);
	}
	{
		// Phase 3: Consider cases 3R and 4R and generate seedlings for
		// case 4F
		Timer _t(cout, "Seeded quality search Phase 3: ", timing);
		// BacktrackManager to search for seedlings for case 4F
		BacktrackManager<TStr> btf(
				ebwtFw, params,
		        0, 0,                  // 5, 3depth
                s3,                    // unrevOff
                (seedMms > 1)? s3 : s, // 1revOff
                (seedMms > 2)? s3 : s, // 2revOff
                (seedMms > 3)? s3 : s, // 3revOff
		        0, 0,                  // itop, ibot
		        qualCutoff,            // qualThresh (none)
		        maxBts,                // max backtracks
		        seedMms,               // reportSeedlings (do)
		        pamFw,                 // seedlings
			    NULL,                  // mutations
		        verbose,               // verbose
		        true,                  // oneHit
			    seed+3,                // seed
			    &os);
		// BacktrackManager to search for hits for case 4R by extending
		// the partial alignments found in Phase 2
		BacktrackManager<TStr> btr(
				ebwtFw, params,
		        0, 0,    // 5, 3depth
		        s,       // unrevOff
		        s,       // 1revOff
		        s,       // 2revOff
		        s,       // 3revOff
		        0, 0,    // itop, ibot
		        qualCutoff, // qualThresh
		        maxBts,  // max backtracks
		        0,       // reportSeedlings (don't)
		        NULL,    // seedlings
			    NULL,    // mutations
		        verbose, // verbose
		        true,    // oneHit
			    seed+4,  // seed
			    &os);
		// The half-and-half BacktrackManager
		BacktrackManager<TStr> btr2(
				ebwtFw, params,
		        s5, s,
		        0,                      // unrevOff
		        (seedMms <= 2)? s5 : 0, // 1revOff
		        (seedMms < 3) ? s : s5, // 2revOff
		        s,                      // 3revOff
		        0, 0,    // itop, ibot
		        qualCutoff, // qualThresh
		        maxBts,  // max backtracks
		        0,       // reportSeedlings (don't)
		        NULL,    // seedlings
			    NULL,    // mutations
		        verbose, // verbose
		        true,    // oneHit
			    seed+5,  // seed
			    &os,
			    true,    // considerQuals
			    true);   // halfAndHalf
		vector<PartialAlignment> pals;
	    while(true) {
			GET_READ(patsrc);
			{
				MUTEX_LOCK(doneMaskLock);
		    	assert_lt(patid, doneMask.capacity());
		    	assert_lt(patid, doneMask.size());
		    	bool done = doneMask[patid];
		    	MUTEX_UNLOCK(doneMaskLock);
				if(done) continue;
	    	}
			params.setFw(false);  // looking at reverse-comp strand
			btr.setQuery(&patRc, &qualRc, &name);

			// Given the partial alignments generated in phase 2, check
			// for hits for case 4R
			uint32_t plen = length(patRc);
			uint32_t qs = min<uint32_t>(plen, s);
			uint32_t qs3 = qs >> 1;
			uint32_t qs5 = (qs >> 1) + (qs & 1);

			// Get all partial alignments for this read's reverse
			// complement
			pals.clear();
			if(pamRc != NULL) {
				pamRc->getPartials(patid, pals);
			}
			bool hit = false;
			if(pals.size() > 0) {
				// Partial alignments exist - extend them
				// Set up special seed bounds
				if(qs < s) {
					btr.setOffs(0, 0, qs, qs, qs, qs);
				}
				for(size_t i = 0; i < pals.size(); i++) {
					String<QueryMutation> muts;
					uint8_t oldQuals =
						PartialAlignmentManager::toMutsString(
								pals[i], patRc, qualRc, muts);

					// Set the backtracking thresholds appropriately
					// Now begin the backtracking, treating the first
					// 24 bases as unrevisitable
					ASSERT_ONLY(uint64_t numHits = sink.numHits());
					ASSERT_ONLY(TStr tmp = patRc);
					btr.setMuts(&muts);
					hit = btr.backtrack(oldQuals);
					btr.setMuts(NULL);
					assert_eq(tmp, patRc); // assert mutations were undone
					assert(hit  || numHits == sink.numHits());
					assert(!hit || numHits <  sink.numHits());
					if(hit) {
						// The reverse complement hit, so we're done with this
						// read
						MUTEX_LOCK(doneMaskLock);
						doneMask[patid] = true;
						MUTEX_UNLOCK(doneMaskLock);
						// Got a hit; stop processing partial
						// alignments
						break;
					}
				} // Loop over partial alignments
				// Restore usual seed bounds
				if(qs < s) {
					btr.setOffs(0, 0, s, s, s, s);
				}
			}

			// Case 4R yielded a hit; mark this pattern as done and
			// continue to next pattern
	    	if(hit) continue;

	    	// If we're in two-mismatch mode, then now is the time to
	    	// try the final case that might apply to the reverse
	    	// complement pattern: 1 mismatch in each of the 3' and 5'
	    	// halves of the seed.
	    	bool gaveUp = false;
	    	if(seedMms >= 2) {
				btr2.setQuery(&patRc, &qualRc, &name);
				ASSERT_ONLY(uint64_t numHits = sink.numHits());
				// Set up special seed bounds
				if(qs < s) {
					btr2.setOffs(qs5, qs,
					             0,                         // unrevOff
					             (seedMms <= 2)? qs5 : 0,   // 1revOff
					             (seedMms < 3 )? qs  : qs5, // 2revOff
					             qs);                       // 3revOff
				}
				bool hit = btr2.backtrack();
				// Restore usual seed bounds
				if(qs < s) {
					btr2.setOffs(s5, s,
					             0,                         // unrevOff
					             (seedMms <= 2)? s5 : 0,    // 1revOff
					             (seedMms < 3 )? s  : s5,   // 2revOff
					             s);                        // 3revOff
				}
				if(btr2.numBacktracks() == btr2.maxBacktracks()) {
					gaveUp = true;
				}
				btr2.resetNumBacktracks();
				assert(hit  || numHits == sink.numHits());
				assert(!hit || numHits <  sink.numHits());
				if(hit) {
					MUTEX_LOCK(doneMaskLock);
					doneMask[patid] = true;
					MUTEX_UNLOCK(doneMaskLock);
					continue;
				}
	    	}

#ifndef NDEBUG
			// The reverse-complement version of the read doesn't hit
	    	// at all!  Check with the oracle to make sure it agrees.
	    	if(!gaveUp) {
	    		ASSERT_NO_HITS_RC(true);
	    	}
#endif

			// If we reach here, then cases 1F, 2F, 3F, 1R, 2R, 3R and
			// 4R have been eliminated leaving only 4F.
			params.setFw(true);  // looking at forward strand
			btf.setQuery(&patFw, &qualFw, &name);
			btf.setQlen(seedLen); // just look at the seed
			// Set up special seed bounds
			if(qs < s) {
				btf.setOffs(0, 0,
				            qs3,
				            (seedMms > 1)? qs3 : qs,
				            (seedMms > 2)? qs3 : qs,
				            (seedMms > 3)? qs3 : qs);
			}
			// Do a 12/24 seedling backtrack on the forward read
			// using the normal index.  This will find seedlings
			// for case 4F
			btf.backtrack();
			// Set up special seed bounds
			if(qs < s) {
				btf.setOffs(0, 0,
				            s3,
				            (seedMms > 1)? s3 : s,
				            (seedMms > 2)? s3 : s,
				            (seedMms > 3)? s3 : s);
			}
#ifndef NDEBUG
			if(seedMms > 0) {
				vector<PartialAlignment> partials;
				pamFw->getPartials(patid, partials);
				if(hit) assert_gt(partials.size(), 0);
				for(size_t i = 0; i < partials.size(); i++) {
					uint32_t pos0 = partials[i].entry.pos0;
					assert_lt(pos0, s5);
					uint8_t oldChar = (uint8_t)patFw[pos0];
					assert_neq(oldChar, partials[i].entry.char0);
					if(partials[i].entry.pos1 != 0xff) {
						uint32_t pos1 = partials[i].entry.pos1;
						assert_lt(pos1, s5);
						oldChar = (uint8_t)patFw[pos1];
						assert_neq(oldChar, partials[i].entry.char1);
						if(partials[i].entry.pos2 != 0xff) {
							uint32_t pos2 = partials[i].entry.pos2;
							assert_lt(pos2, s5);
							oldChar = (uint8_t)patFw[pos2];
							assert_neq(oldChar, partials[i].entry.char2);
						}
					}
				}
			}
#endif
	    }
	}
	// Some with the reverse-complement partial alignments
	if(pamRc != NULL) {
		delete pamRc;
	}
	// Unload forward index and load mirror index
	SWITCH_TO_BW_INDEX();
	patsrc->reset();
	params.setEbwtFw(false);
	{
		// Phase 4: Consider case 4F
		Timer _t(cout, "Seeded quality search Phase 4: ", timing);
		// BacktrackManager to search for hits for case 4F by extending
		// the partial alignments found in Phase 3
		BacktrackManager<TStr> btf(
				ebwtBw, params,
		        0, 0,    // 5, 3depth
                s,       // unrevOff
                s,       // 1revOff
                s,       // 2revOff
                s,       // 3revOff
		        0, 0,    // itop, ibot
		        qualCutoff, // qualThresh
		        maxBts,  // max backtracks
		        0,       // reportSeedlings (don't)
		        NULL,    // seedlings
			    NULL,    // mutations
		        verbose, // verbose
		        true,    // oneHit
		        seed+6,  // seed
		        &os);
		// Half-and-half BacktrackManager for forward read
		BacktrackManager<TStr> btf2(
				ebwtBw, params,
		        s5, s,   // 5, 3depth
		        0,                      // unrevOff
		        (seedMms <= 2)? s5 : 0, // 1revOff
		        (seedMms <  3)? s : s5, // 2revOff
		        s,                      // 3revOff
		        0, 0,    // itop, ibot
		        qualCutoff, // qualThresh
		        maxBts,  // max backtracks
		        0,       // reportSeedlings (don't)
		        NULL,    // seedlings
			    NULL,    // mutations
		        verbose, // verbose
		        true,    // oneHit
		        seed+7,  // seed
		        &os,
		        true,    // considerQuals
		        true);   // halfAndHalf
		params.setFw(true);  // looking only at forward strand
		vector<PartialAlignment> pals;
	    while(true) {
			GET_READ_FW(patsrc);
			{
				MUTEX_LOCK(doneMaskLock);
		    	assert_lt(patid, doneMask.capacity());
		    	assert_lt(patid, doneMask.size());
		    	bool done = doneMask[patid];
		    	MUTEX_UNLOCK(doneMaskLock);
				if(done) continue;
	    	}
			params.setFw(true);
			btf.setQuery(&patFw, &qualFw, &name);

			// Given the seedlines generated in phase 3, check for hits
			// for case 4F
			uint32_t plen = length(patFw);
			uint32_t qs = min<uint32_t>(plen, s);
			uint32_t qs5 = (qs >> 1) + (qs & 1);

			// Get all partial alignments for this read's reverse
			// complement
			pals.clear();
			if(pamFw != NULL) {
				pamFw->getPartials(patid, pals);
			}
			bool hit = false;
			if(pals.size() > 0) {
				// Partial alignments exist - extend them
				// Set up special seed bounds
				if(qs < s) {
					btf.setOffs(0, 0, qs, qs, qs, qs);
				}
				for(size_t i = 0; i < pals.size(); i++) {
					String<QueryMutation> muts;
					uint8_t oldQuals =
						PartialAlignmentManager::toMutsString(
								pals[i], patFw, qualFw, muts);

					// Set the backtracking thresholds appropriately
					// Now begin the backtracking, treating the first
					// 24 bases as unrevisitable
					ASSERT_ONLY(uint64_t numHits = sink.numHits());
					ASSERT_ONLY(TStr tmp = patFw);
					btf.setMuts(&muts);
					hit = btf.backtrack(oldQuals);
					btf.setMuts(NULL);
					assert_eq(tmp, patFw); // assert mutations were undone
					assert(hit  || numHits == sink.numHits());
					assert(!hit || numHits <  sink.numHits());
					if(hit) {
						// The reverse complement hit, so we're done with this
						// read
						MUTEX_LOCK(doneMaskLock);
						doneMask[patid] = true;
						MUTEX_UNLOCK(doneMaskLock);
						// Got a hit; stop processing partial
						// alignments
						break;
					}
				} // Loop over partial alignments
				// Restore usual seed bounds
				if(qs < s) {
					btf.setOffs(0, 0, s, s, s, s);
				}
			}

			// Case 4F yielded a hit; mark this pattern as done and
			// continue to next pattern
	    	if(hit) continue;

	    	// If we're in two-mismatch mode, then now is the time to
	    	// try the final case that might apply to the forward
	    	// pattern: 1 mismatch in each of the 3' and 5' halves of
	    	// the seed.
	    	bool gaveUp = false;
	    	if(seedMms >= 2) {
				ASSERT_ONLY(uint64_t numHits = sink.numHits());
				btf2.setQuery(&patFw, &qualFw, &name);
				// Set special seed bounds
				if(qs < s) {
					btf2.setOffs(qs5, qs,
					             0,                        // unrevOff
					             (seedMms <= 2)? qs5 : 0,  // 1revOff
					             (seedMms < 3)? qs : qs5,  // 2revOff
					             qs);                      // 3revOff
				}
				bool hit = btf2.backtrack();
				// Restore usual seed bounds
				if(qs < s) {
					btf2.setOffs(s5, s,
					             0,                        // unrevOff
					             (seedMms <= 2)? s5 : 0,   // 1revOff
					             (seedMms < 3)? s : s5,    // 2revOff
					             s);                       // 3revOff
				}
				if(btf2.numBacktracks() == btf2.maxBacktracks()) {
					gaveUp = true;
				}
				btf2.resetNumBacktracks();
				assert(hit  || numHits == sink.numHits());
				assert(!hit || numHits <  sink.numHits());
				if(hit) {
					MUTEX_LOCK(doneMaskLock);
					doneMask[patid] = true;
					MUTEX_UNLOCK(doneMaskLock);
					continue;
				}
	    	}

#ifndef NDEBUG

			// The forward version of the read doesn't hit at all!
			// Check with the oracle to make sure it agrees.
	    	if(!gaveUp) {
	    		ASSERT_NO_HITS_FW(false);
			}
#endif
	    } // while(patsrc.hasMorePatterns()...
	} // end of Phase 4
	if(pamFw != NULL) {
		delete pamFw;
	}
}

/**
 * Try to find the Bowtie index specified by the user.  First try the
 * exact path given by the user.  Then try the user-provided string
 * appended onto the path of the "indexes" subdirectory below this
 * executable, then try the provided string appended onto
 * "$BOWTIE_INDEXES/".
 */
static string adjustEbwtBase(const string& ebwtFileBase) {
	string str = ebwtFileBase;
	ifstream in;
	if(verbose) cout << "Trying " << str << endl;
	in.open((str + ".1.ebwt").c_str(), ios_base::in | ios::binary);
	if(!in.is_open()) {
		if(verbose) cout << "  didn't work" << endl;
		in.close();
		str = argv0;
		size_t st = str.find_last_of("/\\");
		if(st != string::npos) {
			str.erase(st);
			str += "/indexes/";
		} else {
			str = "indexes/";
		}
		str += ebwtFileBase;
		if(verbose) cout << "Trying " << str << endl;
		in.open((str + ".1.ebwt").c_str(), ios_base::in | ios::binary);
		if(!in.is_open()) {
			if(verbose) cout << "  didn't work" << endl;
			in.close();
			if(getenv("BOWTIE_INDEXES") != NULL) {
				str = string(getenv("BOWTIE_INDEXES")) + "/" + ebwtFileBase;
				if(verbose) cout << "Trying " << str << endl;
				in.open((str + ".1.ebwt").c_str(), ios_base::in | ios::binary);
				if(!in.is_open()) {
					if(verbose) cout << "  didn't work" << endl;
					in.close();
				}
			}
		}
	}
	if(!in.is_open()) {
		cerr << "Could not locate a Bowtie index corresponding to basename \"" << ebwtFileBase << "\"" << endl;
		exit(1);
	}
	return str;
}

template<typename TStr>
static void driver(const char * type,
                   const string& ebwtFileBase,
                   const string& query,
                   const vector<string>& queries,
                   const string& outfile)
{
	// Vector of the reference sequences; used for sanity-checking
	vector<String<Dna5> > os;
	// Read reference sequences from the command-line or from a FASTA file
	if(sanityCheck && !origString.empty()) {
		// Determine if it's a file by looking at whether it has a FASTA-like
		// extension
		if(origString.substr(origString.length()-6) == ".fasta" ||
		   origString.substr(origString.length()-4) == ".mfa"   ||
		   origString.substr(origString.length()-4) == ".fas"   ||
		   origString.substr(origString.length()-4) == ".fna"   ||
		   origString.substr(origString.length()-3) == ".fa")
		{
			// Read fasta file
			vector<string> origFiles;
			tokenize(origString, ",", origFiles);
			readSequenceFiles<String<Dna5>, Fasta>(origFiles, os);
		} else {
			// Read sequence
			readSequenceString(origString, os);
		}
	}
	// Adjust
	string adjustedEbwtFileBase = adjustEbwtBase(ebwtFileBase);
	// Seed random number generator
	srand(seed);
	// Create a pattern source for the queries
	PatternSource *patsrc = NULL;
	if(nsPolicy == NS_TO_NS && !maqLike) {
		maxNs = min<int>(maxNs, mismatches);
	}
	switch(format) {
		case FASTA:
			patsrc = new FastaPatternSource (queries, false,
			                                 patDumpfile, trim3, trim5,
			                                 nsPolicy, maxNs);
			break;
		case RAW:
			patsrc = new RawPatternSource   (queries, false,
			                                 patDumpfile, trim3, trim5,
			                                 nsPolicy, maxNs);
			break;
		case FASTQ:
			patsrc = new FastqPatternSource (queries, false,
			                                 patDumpfile, trim3, trim5,
			                                 nsPolicy, solexa_quals,
			                                 maxNs);
			break;
		case CMDLINE:
			patsrc = new VectorPatternSource(queries, false,
			                                 patDumpfile, trim3,
			                                 trim5, nsPolicy, maxNs);
			break;
		case RANDOM:
			patsrc = new RandomPatternSource(2000000, lenRandomReads, patDumpfile, seed);
			break;
		default: assert(false);
	}
	if(skipSearch) return;
	// Open hit output file
	ostream *fout;
	if(!outfile.empty()) {
		fout = new ofstream(outfile.c_str(), ios::binary);
	} else {
		fout = &cout;
	}
	// Initialize Ebwt object and read in header
    Ebwt<TStr> ebwt(adjustedEbwtFileBase, /* overriding: */ offRate, verbose, sanityCheck);
    assert_geq(ebwt.eh().offRate(), offRate);
    Ebwt<TStr>* ebwtBw = NULL;
    // We need the mirror index if mismatches are allowed
    if(mismatches > 0 || maqLike) {
    	ebwtBw = new Ebwt<TStr>(adjustedEbwtFileBase + ".rev", /* overriding: */ offRate, verbose, sanityCheck);
    }
	if(sanityCheck && !os.empty()) {
		// Sanity check number of patterns and pattern lengths in Ebwt
		// against original strings
		assert_eq(os.size(), ebwt.nPat());
		for(size_t i = 0; i < os.size(); i++) {
			assert_eq(length(os[i]), ebwt.plen()[i]);
		}
	}
    // Load rest of (vast majority of) Ebwt into memory
	if(!maqLike) {
		Timer _t(cout, "Time loading Ebwt: ", timing);
	    ebwt.loadIntoMemory();
	}
	// Sanity-check the restored version of the Ebwt
	if(sanityCheck && !os.empty()) {
		if(maqLike) ebwt.loadIntoMemory();
		ebwt.checkOrigs(os, false);
		if(maqLike) ebwt.evictFromMemory();
	}
    // If sanity-check is enabled and an original text string
    // was specified, sanity-check the Ebwt by confirming that
    // the unpermuted version equals the original.
	// NOTE: Disabled since, with fragments, it's no longer possible to do
	// this straightforwardly with the os vector.  Rather, we need to either
	// split each element of the os vector on Ns, or we need to read the
	// references in differently.  The former seems preferable.
//	if(!maqLike && sanityCheck && !os.empty()) {
//		TStr rs; ebwt.restore(rs);
//		TStr joinedo = Ebwt<TStr>::join(os, ebwt.eh().chunkRate(), seed);
//		assert_leq(length(rs), length(joinedo));
//		assert_geq(length(rs) + ebwt.eh().chunkLen(), length(joinedo));
//		for(size_t i = 0; i < length(rs); i++) {
//			if(rs[i] != joinedo[i]) {
//				cout << "At character " << i << " of " << length(rs) << endl;
//			}
//			assert_eq(rs[i], joinedo[i]);
//		}
//	}
	{
		Timer _t(cout, "Time searching: ", timing);
		// Set up hit sink; if sanityCheck && !os.empty() is true,
		// then instruct the sink to "retain" hits in a vector in
		// memory so that we can easily sanity check them later on
		HitSink *sink;
		switch(outType) {
			case FULL:
				sink = new VerboseHitSink(
						*fout,
						&ebwt.refnames());
				break;
			case CONCISE:
				sink = new ConciseHitSink(
						*fout,
						reportOpps,
						&ebwt.refnames());
				break;
			case NONE:
				sink = new StubHitSink();
				break;
			default:
				cerr << "Invalid output type: " << outType << endl;
				exit(1);
		}
		EbwtSearchStats<TStr> stats;
		if(maqLike) {
			seededQualCutoffSearch(seedLen,
			                       qualThresh,
			                       seedMms,
			                       *patsrc,
			                       *sink,
			                       stats,
			                       ebwt,    // forward index
			                       *ebwtBw, // mirror index (not optional)
			                       os);     // references, if available
		}
		else if(mismatches > 0) {
			if(mismatches == 1) {
				mismatchSearch(*patsrc, *sink, stats, ebwt, *ebwtBw, os);
			} else if(mismatches == 2 || mismatches == 3) {
				twoOrThreeMismatchSearch(*patsrc, *sink, stats, ebwt, *ebwtBw, os, mismatches == 2);
			} else {
				cerr << "Error: " << mismatches << " is not a supported number of mismatches" << endl;
				exit(1);
			}
		} else {
			// Search without mismatches
			exactSearch(*patsrc, *sink, stats, ebwt, os);
		}
	    sink->finish(); // end the hits section of the hit file
	    if(printStats) {
		    // Write some high-level searching parameters and inputs
	    	// to the hit file
		    sink->out() << "Binary name: " << argv0 << endl;
		    sink->out() << "  Checksum: " << (uint64_t)(EBWT_SEARCH_HASH) << endl;
		    sink->out() << "Ebwt file base: " << adjustedEbwtFileBase << endl;
			sink->out() << "Sanity checking: " << (sanityCheck? "on":"off") << endl;
			sink->out() << "Verbose: " << (verbose? "on":"off") << endl;
		    sink->out() << "Queries: " << endl;
		    for(size_t i = 0; i < queries.size(); i++) {
		    	sink->out() << "  " << queries[i] << endl;
		    }
		    //params.write(sink->out()); // write searching parameters
		    stats.write(sink->out());  // write searching statistics
		    _t.write(sink->out());     // write timing info
	    }
	    sink->flush();
		if(!outfile.empty()) {
			((ofstream*)fout)->close();
		}
		delete sink;
	}
}

/**
 * main function.  Parses command-line arguments.
 */
int main(int argc, char **argv) {
	string ebwtFile;  // read serialized Ebwt from this file
	string query;   // read query string(s) from this file
	vector<string> queries;
	string outfile; // write query results to this file
	parseOptions(argc, argv);
	argv0 = argv[0];
	if(showVersion) {
		cout << argv0 << " version " << BOWTIE_VERSION << endl;
		cout << "Built on " << BUILD_HOST << endl;
		cout << BUILD_TIME << endl;
		cout << "Compiler: " << COMPILER_VERSION << endl;
		cout << "Options: " << COMPILER_OPTIONS << endl;
		cout << "Sizeof {int, long, long long, void*}: {" << sizeof(int)
		     << ", " << sizeof(long) << ", " << sizeof(long long)
		     << ", " << sizeof(void *) << "}" << endl;
		cout << "Source hash: " << EBWT_SEARCH_HASH << endl;
		return 0;
	}
	Timer _t(cout, "Overall time: ", timing);

	// Get input filename
	if(optind >= argc) {
		cerr << "No input sequence, query, or output file specified!" << endl;
		printUsage(cerr);
		return 1;
	}
	ebwtFile = argv[optind++];

	// Get query filename
	if(optind >= argc) {
		cerr << "No query or output file specified!" << endl;
		printUsage(cerr);
		return 1;
	}
	query = argv[optind++];

	// Tokenize the list of query files
	tokenize(query, ",", queries);
	if(queries.size() < 1) {
		cerr << "Tokenized query file list was empty!" << endl;
		printUsage(cerr);
		return 1;
	}

	// Get output filename
	if(optind < argc) {
		outfile = argv[optind++];
	}

	// Optionally summarize
	if(verbose) {
		cout << "Input ebwt file: \"" << ebwtFile << "\"" << endl;
		cout << "Query inputs (DNA, " << file_format_names[format] << "):" << endl;
		for(size_t i = 0; i < queries.size(); i++) {
			cout << "  " << queries[i] << endl;
		}
		cout << "Output file: \"" << outfile << "\"" << endl;
		cout << "Local endianness: " << (currentlyBigEndian()? "big":"little") << endl;
		cout << "Sanity checking: " << (sanityCheck? "enabled":"disabled") << endl;
	#ifdef NDEBUG
		cout << "Assertions: disabled" << endl;
	#else
		cout << "Assertions: enabled" << endl;
	#endif
	}
	if(ipause) {
		cout << "Press key to continue..." << endl;
		getchar();
	}
	driver<String<Dna, Alloc<> > >("DNA", ebwtFile, query, queries, outfile);
#ifdef BOWTIE_PTHREADS
	pthread_exit(NULL);
#else
	return 0;
#endif
}
