/*
 * Copyright (C) 2018 by Benedict Paten (benedictpaten@gmail.com)
 *
 * Released under the MIT license, see LICENSE.txt
 */

#include <getopt.h>
#include <stdio.h>
#include <ctype.h>
#include <memory.h>
#include <hashTableC.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "marginVersion.h"

#include "margin.h"
#include "htsIntegration.h"
#include "helenFeatures.h"


/*
 * Main functions
 */

void usage() {
    fprintf(stderr, "usage: marginPolish <BAM_FILE> <ASSEMBLY_FASTA> <PARAMS> [options]\n");
    fprintf(stderr, "Version: %s \n\n", MARGIN_POLISH_VERSION_H);
    fprintf(stderr, "Polishes the ASSEMBLY_FASTA using alignments in BAM_FILE.\n");

    fprintf(stderr, "\nRequired arguments:\n");
    fprintf(stderr, "    BAM_FILE is the alignment of reads to the assembly (or reference).\n");
    fprintf(stderr, "    ASSEMBLY_FASTA is the reference sequence BAM file in fasta format.\n");
    fprintf(stderr, "    PARAMS is the file with marginPolish parameters.\n");

    fprintf(stderr, "\nDefault options:\n");
    fprintf(stderr, "    -h --help                : Print this help screen\n");
    fprintf(stderr, "    -a --logLevel            : Set the log level [default = info]\n");
    # ifdef _OPENMP
    fprintf(stderr, "    -t --threads             : Set number of concurrent threads [default = 1]\n");
    #endif
    fprintf(stderr, "    -o --outputBase          : Name to use for output files [default = 'output']\n");
    fprintf(stderr, "    -r --region              : If set, will only compute for given chromosomal region.\n");
    fprintf(stderr, "                                 Format: chr:start_pos-end_pos (chr3:2000-3000).\n");
    fprintf(stderr, "    -p --depth               : Will override the downsampling depth set in PARAMS.\n");
    fprintf(stderr, "    -2 --diploid             : Will perform diploid phasing.\n");

    # ifdef _HDF5
    fprintf(stderr, "\nHELEN feature generation options:\n");
    fprintf(stderr, "    -f --produceFeatures     : output splitRleWeight features for HELEN.\n");
    fprintf(stderr, "    -F --featureType         : output specific feature type for HELEN (overwrites -f).  Valid types:\n");
    fprintf(stderr, "                                 splitRleWeight:   [default] run lengths split into chunks\n");
    fprintf(stderr, "                                 channelRleWeight: run lengths split into per-nucleotide channels\n");
    fprintf(stderr, "                                 simpleWeight:     weighted likelihood from POA nodes (non-RLE)\n");
    fprintf(stderr, "                                 diploidRleWeight: [default] produces diploid features \n");
    fprintf(stderr, "    -L --splitRleWeightMaxRL : max run length (for 'splitRleWeight' and 'channelRleWeight' types) \n");
    fprintf(stderr, "                                 [split default = %d, channel default = %d, diploid default = %d]\n",
            POAFEATURE_SPLIT_MAX_RUN_LENGTH_DEFAULT, POAFEATURE_CHANNEL_MAX_RUN_LENGTH_DEFAULT, POAFEATURE_DIPLOID_MAX_RUN_LENGTH_DEFAULT);
    fprintf(stderr, "    -u --trueReferenceBam    : true reference aligned to ASSEMBLY_FASTA, for HELEN\n");
    fprintf(stderr, "                               features.  Setting this parameter will include labels\n");
    fprintf(stderr, "                               in output.  If -2/--diploid is set, this parameter must\n");
    fprintf(stderr, "                               contain two comma-separated values\n");
    # endif

    fprintf(stderr, "\nMiscellaneous supplementary output options:\n");
    fprintf(stderr, "    -d --outputPoaDot        : Output base to write out the poa as DOT file [default = NULL]\n");
    fprintf(stderr, "    -i --outputRepeatCounts  : Output base to write out the repeat counts [default = NULL]\n");
    fprintf(stderr, "    -j --outputPoaTsv        : Output base to write out the poa as TSV file [default = NULL]\n");
    fprintf(stderr, "    -m --outputHaplotypeBAM  : Output base to write out phased BAMs [default = NULL]\n");
    fprintf(stderr, "    -n --outputHaplotypeReads: Output base to write out phased reads [default = NULL]\n");
    fprintf(stderr, "\n");
}

char *getFileBase(char *base, char *defawlt) {
    struct stat fileStat;
    int64_t rc = stat(base, &fileStat);
    if (S_ISDIR(fileStat.st_mode)) {
        if (optarg[strlen(base) - 1] == '/') optarg[strlen(base) - 1] = '\0';
        return stString_print("%s/%s", base, defawlt);
    } else {
        return stString_copy(base);
    }
}



//TODO move these to a better spot
stHash *parseReferenceSequences(char *referenceFastaFile) {
    /*
     * Get hash of reference sequence names in fasta to their sequences, doing some munging on the sequence names.
     */
    st_logCritical("> Parsing reference sequences from file: %s\n", referenceFastaFile);
    FILE *fh = fopen(referenceFastaFile, "r");
    stHash *referenceSequences = fastaReadToMap(fh);  //valgrind says blocks from this allocation are "still reachable"
    fclose(fh);
    // log names and transform (if necessary)
    stList *refSeqNames = stHash_getKeys(referenceSequences);
    int64_t origRefSeqLen = stList_length(refSeqNames);
    st_logDebug("\tReference contigs: \n");
    for (int64_t i = 0; i < origRefSeqLen; ++i) {
        char *fullRefSeqName = (char *) stList_get(refSeqNames, i);
        st_logDebug("\t\t%s\n", fullRefSeqName);
        char refSeqName[128] = "";
        if (sscanf(fullRefSeqName, "%s", refSeqName) == 1 && !stString_eq(fullRefSeqName, refSeqName)) {
            // this transformation is necessary for cases where the reference has metadata after the contig name:
            // >contig001 length=1000 date=1999-12-31
            char *newKey = stString_copy(refSeqName);
            char *refSeq = stHash_search(referenceSequences, fullRefSeqName);
            stHash_insert(referenceSequences, newKey, refSeq);
            stHash_removeAndFreeKey(referenceSequences, fullRefSeqName);
            st_logDebug("\t\t\t-> %s\n", newKey);
        }
    }
    stList_destruct(refSeqNames);

    return referenceSequences;
}

void handleMerge(BamChunker *bamChunker, char **chunkResults, int numThreads, Params *params, FILE *polishedReferenceOutFh) {

    // prep for merge
    assert(bamChunker->chunkCount > 0);
    int64_t contigStartIdx = 0;
    char *referenceSequenceName = stString_copy(bamChunker_getChunk(bamChunker, 0)->refSeqName);
    int64_t  lastReportedPercentage = 0;
    time_t mergeStartTime = time(NULL);

    // merge chunks
    st_logCritical("> Merging polished reference strings from %"PRIu64" chunks.\n", bamChunker->chunkCount);

    // find which chunks belong to each contig, merge each contig threaded, write out
    for (int64_t chunkIdx = 1; chunkIdx <= bamChunker->chunkCount; chunkIdx++) {

        // we encountered the last chunk in the contig (end of list or new refSeqName)
        if (chunkIdx == bamChunker->chunkCount || !stString_eq(referenceSequenceName,
                                                               bamChunker_getChunk(bamChunker, chunkIdx)->refSeqName)) {

            // generate and save sequence
            char *contigSequence = mergeContigChunksThreaded(chunkResults, contigStartIdx, chunkIdx, numThreads,
                                                             params, referenceSequenceName);
            fastaWrite(contigSequence, referenceSequenceName, polishedReferenceOutFh);

            // log progress
            int64_t currentPercentage = (int64_t) (100 * chunkIdx / bamChunker->chunkCount);
            if (currentPercentage != lastReportedPercentage) {
                lastReportedPercentage = currentPercentage;
                int64_t timeTaken = (int64_t) (time(NULL) - mergeStartTime);
                int64_t secondsRemaining = (int64_t) floor(1.0 * timeTaken / currentPercentage * (100 - currentPercentage));
                char *timeDescriptor = (secondsRemaining == 0 && currentPercentage <= 50 ?
                                        stString_print("unknown") : getTimeDescriptorFromSeconds(secondsRemaining));
                st_logCritical("> Merging %2"PRId64"%% complete (%"PRId64"/%"PRId64").  Estimated time remaining: %s\n",
                               currentPercentage, chunkIdx, bamChunker->chunkCount, timeDescriptor);
                free(timeDescriptor);
            }

            // Clean up
            free(contigSequence);
            free(referenceSequenceName);

            // Reset for next reference sequence
            if (chunkIdx != bamChunker->chunkCount) {
                contigStartIdx = chunkIdx;
                referenceSequenceName = stString_copy(bamChunker_getChunk(bamChunker, chunkIdx)->refSeqName);
            }
        }
        // nothing to do otherwise, just wait until end or new contig
    }

}


void handleDiploidMerge(BamChunker *bamChunker, char **chunkResultsH1, char **chunkResultsH2, stSet **readsInH1,
        stSet **readsInH2, int numThreads, Params *params, FILE *polishedReferenceOutFhH1,
        FILE *polishedReferenceOutFhH2) {

    // prep for merge
    assert(bamChunker->chunkCount > 0);
    int64_t contigStartIdx = 0;
    char *referenceSequenceName = stString_copy(bamChunker_getChunk(bamChunker, 0)->refSeqName);
    int64_t  lastReportedPercentage = 0;
    time_t mergeStartTime = time(NULL);

    // merge chunks
    st_logCritical("> Merging diploid polished reference strings from %"PRIu64" chunks.\n", bamChunker->chunkCount);

    // find which chunks belong to each contig, merge each contig threaded, write out
    for (int64_t chunkIdx = 1; chunkIdx <= bamChunker->chunkCount; chunkIdx++) {

        // we encountered the last chunk in the contig (end of list or new refSeqName)
        if (chunkIdx == bamChunker->chunkCount || !stString_eq(referenceSequenceName,
                                                               bamChunker_getChunk(bamChunker, chunkIdx)->refSeqName)) {

            // generate and save sequence
            char **contigSequences = mergeContigChunksDiploidThreaded(chunkResultsH1, chunkResultsH2,
                    readsInH1, readsInH2, contigStartIdx, chunkIdx, numThreads, params, referenceSequenceName);
            fastaWrite(contigSequences[0], referenceSequenceName, polishedReferenceOutFhH1);
            fastaWrite(contigSequences[1], referenceSequenceName, polishedReferenceOutFhH2);

            // log progress
            int64_t currentPercentage = (int64_t) (100 * chunkIdx / bamChunker->chunkCount);
            if (currentPercentage != lastReportedPercentage) {
                lastReportedPercentage = currentPercentage;
                int64_t timeTaken = (int64_t) (time(NULL) - mergeStartTime);
                int64_t secondsRemaining = (int64_t) floor(1.0 * timeTaken / currentPercentage * (100 - currentPercentage));
                char *timeDescriptor = (secondsRemaining == 0 && currentPercentage <= 50 ?
                                        stString_print("unknown") : getTimeDescriptorFromSeconds(secondsRemaining));
                st_logCritical("> Merging %2"PRId64"%% complete (%"PRId64"/%"PRId64").  Estimated time remaining: %s\n",
                               currentPercentage, chunkIdx, bamChunker->chunkCount, timeDescriptor);
                free(timeDescriptor);
            }

            // Clean up
            free(contigSequences[0]);
            free(contigSequences[1]);
            free(contigSequences);
            free(referenceSequenceName);

            // Reset for next reference sequence
            if (chunkIdx != bamChunker->chunkCount) {
                contigStartIdx = chunkIdx;
                referenceSequenceName = stString_copy(bamChunker_getChunk(bamChunker, chunkIdx)->refSeqName);
            }
        }
        // nothing to do otherwise, just wait until end or new contig
    }

}

RleString *bamChunk_getReferenceSubstring(BamChunk *bamChunk, stHash *referenceSequences, Params *params) {
    /*
     * Get corresponding substring of the reference for a given bamChunk.
     */
    char *fullReferenceString = stHash_search(referenceSequences, bamChunk->refSeqName);
    if (fullReferenceString == NULL) {
        st_logCritical("> ERROR: Reference sequence missing from reference map: %s \n", bamChunk->refSeqName);
        return NULL;
    }
    int64_t refLen = strlen(fullReferenceString);
    char *referenceString = stString_getSubString(fullReferenceString, bamChunk->chunkBoundaryStart,
                                                  (refLen < bamChunk->chunkBoundaryEnd ? refLen : bamChunk->chunkBoundaryEnd) - bamChunk->chunkBoundaryStart);

    RleString *rleRef = params->polishParams->useRunLengthEncoding ?
                        rleString_construct(referenceString) : rleString_construct_no_rle(referenceString);
    free(referenceString);

    return rleRef;
}


uint64_t *getPaddedHaplotypeString(uint64_t *hap, stGenomeFragment *gf, BubbleGraph *bg, Params *params) {
    /*
     * Pads a haplotype string from the genome fragment to account for any missing prefix or suffix.
     */
    uint64_t *paddedHap = bubbleGraph_getConsensusPath(bg, params->polishParams);

    for(uint64_t i=0; i<gf->length; i++) {
        paddedHap[i+gf->refStart] = hap[i];
    }

    return paddedHap;
}


int main(int argc, char *argv[]) {

    // Parameters / arguments
    char *logLevelString = stString_copy("critical");
    char *bamInFile = NULL;
    char *paramsFile = NULL;
    char *referenceFastaFile = NULL;
    char *outputBase = stString_copy("output");
    char *regionStr = NULL;
    int numThreads = 1;
    char *outputRepeatCountBase = NULL;
    char *outputPoaTsvBase = NULL;
    char *outputPoaDotBase = NULL;
    char *outputHaplotypeBamBase = NULL;
    char *outputHaplotypeReadsBase = NULL;
    int64_t maxDepth = -1;
    bool diploid = FALSE;

    // for feature generation
    HelenFeatureType helenFeatureType = HFEAT_NONE;
    bool setDefaultHelenFeature = false;
    char *trueReferenceBam = NULL;
    char *trueReferenceBamHap2 = NULL;
    BamChunker *trueReferenceChunker = NULL;
    bool fullFeatureOutput = FALSE;
    int64_t splitWeightMaxRunLength = 0;
    void **helenHDF5Files = NULL;

    if(argc < 4) {
        free(outputBase);
        free(logLevelString);
        usage();
        return 0;
    }

    bamInFile = stString_copy(argv[1]);
    referenceFastaFile = stString_copy(argv[2]);
    paramsFile = stString_copy(argv[3]);

    // Parse the options
    while (1) {
        static struct option long_options[] = {
                { "logLevel", required_argument, 0, 'a' },
                { "help", no_argument, 0, 'h' },
                # ifdef _OPENMP
                { "threads", required_argument, 0, 't'},
                #endif
                { "outputBase", required_argument, 0, 'o'},
                { "region", required_argument, 0, 'r'},
                { "depth", required_argument, 0, 'p'},
                { "diploid", no_argument, 0, '2'},
                { "produceFeatures", no_argument, 0, 'f'},
                { "featureType", required_argument, 0, 'F'},
                { "trueReferenceBam", required_argument, 0, 'u'},
                { "splitRleWeightMaxRL", required_argument, 0, 'L'},
				{ "outputRepeatCounts", required_argument, 0, 'i'},
				{ "outputPoaTsv", required_argument, 0, 'j'},
				{ "outputPoaDot", required_argument, 0, 'd'},
				{ "outputHaplotypeBAM", required_argument, 0, 'm'},
				{ "outputHaplotypeReads", required_argument, 0, 'n'},
                { 0, 0, 0, 0 } };

        int option_index = 0;
        int key = getopt_long(argc-2, &argv[2], "a:o:v:r:p:fF:u:hL:i:j:d:t:m:n:", long_options, &option_index);

        if (key == -1) {
            break;
        }

        switch (key) {
        case 'a':
            free(logLevelString);
            logLevelString = stString_copy(optarg);
            break;
        case 'h':
            usage();
            return 0;
        case 'o':
            free(outputBase);
            outputBase = getFileBase(optarg, "output");
            break;
        case 'r':
            regionStr = stString_copy(optarg);
            break;
        case 'p':
            maxDepth = atoi(optarg);
            if (maxDepth < 0) {
                st_errAbort("Invalid maxDepth: %s", optarg);
            }
        case 'i':
            outputRepeatCountBase = getFileBase(optarg, "repeatCount");
            break;
        case 'j':
            outputPoaTsvBase = getFileBase(optarg, "poa");
            break;
        case 'd':
            outputPoaDotBase = getFileBase(optarg, "poa");
            break;
        case 'm':
            outputHaplotypeBamBase = getFileBase(optarg, "haplotype");
            break;
        case 'n':
            outputHaplotypeReadsBase = getFileBase(optarg, "haplotype");
            break;
        case 'F':
            if (stString_eqcase(optarg, "simpleWeight") || stString_eqcase(optarg, "simple")) {
                helenFeatureType = HFEAT_SIMPLE_WEIGHT;
            } else if (stString_eqcase(optarg, "rleWeight") || stString_eqcase(optarg, "splitRleWeight") || stString_eqcase(optarg, "split")) {
                helenFeatureType = HFEAT_SPLIT_RLE_WEIGHT;
            } else if (stString_eqcase(optarg, "channelRleWeight") || stString_eqcase(optarg, "channel")) {
                helenFeatureType = HFEAT_CHANNEL_RLE_WEIGHT;
            } else if (stString_eqcase(optarg, "diploidRleWeight") || stString_eqcase(optarg, "diploid")) {
                helenFeatureType = HFEAT_CHANNEL_RLE_WEIGHT;
            } else {
                fprintf(stderr, "Unrecognized featureType for HELEN: %s\n\n", optarg);
                usage();
                return 1;
            }
            break;
        case 'u':
            trueReferenceBam = stString_copy(optarg);
            break;
        case 'f':
            if (helenFeatureType == HFEAT_NONE) {
                setDefaultHelenFeature = true;
            }
            break;
        case 'L':
            splitWeightMaxRunLength = atoi(optarg);
            if (splitWeightMaxRunLength <= 0) {
                st_errAbort("Invalid splitRleWeightMaxRL: %d", splitWeightMaxRunLength);
            }
            break;
        case 't':
            numThreads = atoi(optarg);
            if (numThreads <= 0) {
                st_errAbort("Invalid thread count: %d", numThreads);
            }
            break;
        case '2':
            diploid = TRUE;
            break;
        default:
            usage();
            free(outputBase);
            free(logLevelString);
            free(bamInFile);
            free(referenceFastaFile);
            free(paramsFile);
            if (trueReferenceBam != NULL) free(trueReferenceBam);
            return 0;
        }
    }

    // handle diploid sanity checks
    if (diploid) {
        // true ref
        if (trueReferenceBam != NULL) {
            stList *trueRefParts = stString_splitByString(trueReferenceBam, ",");
            if (stList_length(trueRefParts) != 2) {
                st_errAbort("If --diploid is set, --trueReferenceBam must have two comma-separated values.");
            }
            trueReferenceBam = stList_get(trueRefParts, 0);
            trueReferenceBamHap2 = stList_get(trueRefParts, 1);
            stList_destruct(trueRefParts);
        }
    }

    // sanity check (verify files exist)
    if (access(bamInFile, R_OK ) != 0) {
        st_errAbort("Could not read from file: %s\n", bamInFile);
        char *idx = stString_print("%s.bai", bamInFile);
        if (access(idx, R_OK ) != 0 ) {
            st_errAbort("BAM does not appear to be indexed: %s\n", bamInFile);
        }
        free(idx);
    } else if (access(referenceFastaFile, R_OK ) != 0 ) {
        st_errAbort("Could not read from file: %s\n", referenceFastaFile);
    } else if (access(paramsFile, R_OK ) != 0 ) {
        st_errAbort("Could not read from file: %s\n", paramsFile);
    } else if (trueReferenceBam != NULL && access(trueReferenceBam, R_OK ) != 0 ) {
        st_errAbort("Could not read from file: %s\n", trueReferenceBam);
        char *idx = stString_print("%s.bai", trueReferenceBam);
        if (access(idx, R_OK ) != 0 ) {
            st_errAbort("BAM does not appear to be indexed: %s\n", trueReferenceBam);
        }
        free(idx);
    } else if (trueReferenceBamHap2 != NULL && access(trueReferenceBamHap2, R_OK ) != 0 ) {
        st_errAbort("Could not read from file: %s\n", trueReferenceBamHap2);
        char *idx = stString_print("%s.bai", trueReferenceBamHap2);
        if (access(idx, R_OK ) != 0 ) {
            st_errAbort("BAM does not appear to be indexed: %s\n", trueReferenceBamHap2);
        }
        free(idx);
    }

    // sanitiy check for poa plotting
    if ((outputPoaTsvBase != NULL || outputPoaDotBase != NULL) && regionStr == NULL) {
        st_logCritical("--outputPoaTsv and --outputPoaDot options should only be used for a specific region!\n");
    }

    // Initialization from arguments
    time_t startTime = time(NULL);
    st_setLogLevelFromString(logLevelString);
    free(logLevelString);
    # ifdef _OPENMP
    if (numThreads <= 0) {
        numThreads = 1;
    }
    omp_set_num_threads(numThreads);
    st_logCritical("Running OpenMP with %d threads.\n", omp_get_max_threads());
    # endif

    // feature init
    if (helenFeatureType == HFEAT_NONE && setDefaultHelenFeature) {
        helenFeatureType = diploid ? HFEAT_DIPLOID_RLE_WEIGHT : HFEAT_SPLIT_RLE_WEIGHT;
    }
    if (helenFeatureType != HFEAT_NONE && splitWeightMaxRunLength == 0) {
        switch (helenFeatureType) {
            case HFEAT_SPLIT_RLE_WEIGHT:
                splitWeightMaxRunLength = POAFEATURE_SPLIT_MAX_RUN_LENGTH_DEFAULT;
                break;
            case HFEAT_CHANNEL_RLE_WEIGHT:
                splitWeightMaxRunLength = POAFEATURE_CHANNEL_MAX_RUN_LENGTH_DEFAULT;
                break;
            case HFEAT_DIPLOID_RLE_WEIGHT:
                splitWeightMaxRunLength = POAFEATURE_DIPLOID_MAX_RUN_LENGTH_DEFAULT;
                break;
            default:
                break;
        }
    }

    // Parse parameters
    st_logCritical("> Parsing model parameters from file: %s\n", paramsFile);
    Params *params = params_readParams(paramsFile);

    // update depth (if set)
    if (maxDepth >= 0) {
        st_logCritical("> Changing maxDepth paramter from %"PRId64" to %"PRId64"\n", params->polishParams->maxDepth, maxDepth);
        params->polishParams->maxDepth = (uint64_t) maxDepth;
    }

    // Set no RLE if appropriate feature type is set
    if (helenFeatureType == HFEAT_SIMPLE_WEIGHT) {
        if (params->polishParams->useRunLengthEncoding) {
            st_errAbort("Invalid runLengthEncoding parameter because of HELEN feature type.\n");
        }
    // everthing else requires RLE
    } else if (helenFeatureType != HFEAT_NONE) {
        if (!params->polishParams->useRunLengthEncoding) {
            st_errAbort("Invalid runLengthEncoding parameter because of HELEN feature type.\n");
        }
    }

    // Print a report of the parsed parameters
    if(st_getLogLevel() == debug) {
    	params_printParameters(params, stderr);
    }

    // get reference sequences (and remove cruft after refName)
    stHash *referenceSequences = parseReferenceSequences(referenceFastaFile);

    // Open output files
    char *polishedReferenceOutFile = stString_print((diploid ? "%s.h1.fa" : "%s.fa"), outputBase);
    st_logCritical("> Going to write polished reference in :      %s\n", polishedReferenceOutFile);
    FILE *polishedReferenceOutFh = fopen(polishedReferenceOutFile, "w");
    if (polishedReferenceOutFh == NULL) {
        st_errAbort("Could not open %s for writing!\n", polishedReferenceOutFile);
    }
    char *polishedReferenceOutFileH2 = NULL;
    FILE *polishedReferenceOutFhH2 = NULL;
    if (diploid) {
        polishedReferenceOutFileH2 = stString_print("%s.h2.fa", outputBase);
        st_logCritical("> Going to write polished reference (H2) in : %s\n", polishedReferenceOutFileH2);
        polishedReferenceOutFhH2 = fopen(polishedReferenceOutFileH2, "w");
        if (polishedReferenceOutFhH2 == NULL) {
            st_errAbort("Could not open %s for writing!\n", polishedReferenceOutFileH2);
        }
        free(polishedReferenceOutFileH2);
    }
    free(polishedReferenceOutFile);

    // get chunker for bam.  if regionStr is NULL, it will be ignored
    BamChunker *bamChunker = bamChunker_construct2(bamInFile, regionStr, params->polishParams);
    st_logCritical("> Set up bam chunker with chunk size %i and overlap %i (for region=%s), resulting in %i total chunks\n",
    		   (int)bamChunker->chunkSize, (int)bamChunker->chunkBoundary, regionStr == NULL ? "all" : regionStr,
    		   bamChunker->chunkCount);
    if (bamChunker->chunkCount == 0) {
        st_errAbort("> Found no valid reads!\n");
    }

    // for feature generation
    BamChunker *trueReferenceBamChunker = NULL;
    if (trueReferenceBam != NULL) {
        trueReferenceBamChunker = bamChunker_copyConstruct(bamChunker);
        free(trueReferenceBamChunker->bamFile);
        trueReferenceBamChunker->bamFile = stString_copy(trueReferenceBam);
    }
    #ifdef _HDF5
    if (helenFeatureType != HFEAT_NONE) {
        helenHDF5Files = (void**) openHelenFeatureHDF5FilesByThreadCount(outputBase, numThreads);
    }
    #endif

    // Each chunk produces a char* as output which is saved here
    char **chunkResults = st_calloc(bamChunker->chunkCount, sizeof(char*));

    // diploid info
    char **chunkResultsH1 = chunkResults; //just for naming consistency
    char **chunkResultsH2 = NULL;
    stSet **readSetsH1 = NULL;
    stSet **readSetsH2 = NULL;
    if (diploid) {
        chunkResultsH2 = st_calloc(bamChunker->chunkCount, sizeof(char*));
        readSetsH1 = st_calloc(bamChunker->chunkCount, sizeof(stSet*));
        readSetsH2 = st_calloc(bamChunker->chunkCount, sizeof(stSet*));
    }

    // (may) need to shuffle chunks
    stList *chunkOrder = stList_construct3(0, (void (*)(void*))stIntTuple_destruct);
    for (int64_t i = 0; i < bamChunker->chunkCount; i++) {
        stList_append(chunkOrder, stIntTuple_construct1(i));
    }
    if (params->polishParams->shuffleChunks) {
        stList_shuffle(chunkOrder);
    }

    // multiproccess the chunks, save to results
    int64_t lastReportedPercentage = 0;
    time_t polishStartTime = time(NULL);

    # ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic,1)
    # endif
    for (int64_t i = 0; i < bamChunker->chunkCount; i++) {
        int64_t chunkIdx = stIntTuple_get(stList_get(chunkOrder, i), 0);
        // Time all chunks
        time_t chunkStartTime = time(NULL);

        // Get chunk
        BamChunk *bamChunk = bamChunker_getChunk(bamChunker, chunkIdx);

        // logging
        char *logIdentifier;
        bool logProgress = FALSE;
        int64_t currentPercentage = (int64_t) (100 * i / bamChunker->chunkCount);
        # ifdef _OPENMP
        logIdentifier = stString_print(" T%02d_C%05"PRId64, omp_get_thread_num(), chunkIdx);
        if (omp_get_thread_num() == 0) {
            if (currentPercentage != lastReportedPercentage) {
                logProgress = TRUE;
                lastReportedPercentage = currentPercentage;
            }
        }
        # else
        logIdentifier = stString_copy("");
        if (currentPercentage != lastReportedPercentage) {
            logProgress = TRUE;
            lastReportedPercentage = currentPercentage;
        }
        # endif

        // prints percentage complete and estimated time remaining
        if (logProgress) {
            // log progress
            int64_t timeTaken = (int64_t) (time(NULL) - polishStartTime);
            int64_t secondsRemaining = (int64_t) floor(1.0 * timeTaken / currentPercentage * (100 - currentPercentage));
            char *timeDescriptor = (secondsRemaining == 0 && currentPercentage <= 50 ?
                    stString_print("unknown") : getTimeDescriptorFromSeconds(secondsRemaining));
            st_logCritical("> Polishing %2"PRId64"%% complete (%"PRId64"/%"PRId64").  Estimated time remaining: %s\n",
                    currentPercentage, i, bamChunker->chunkCount, timeDescriptor);
            free(timeDescriptor);
        }

        // Get reference string for chunk of alignment
        char *fullReferenceString = stHash_search(referenceSequences, bamChunk->refSeqName);
        if (fullReferenceString == NULL) {
            st_errAbort("ERROR: Reference sequence missing from reference map: %s. Perhaps the BAM and REF are mismatched?",
                    bamChunk->refSeqName);
        }
        int64_t fullRefLen = strlen(fullReferenceString);
        if (bamChunk->chunkBoundaryStart > fullRefLen) {
            st_errAbort("ERROR: Reference sequence %s has length %"PRId64", chunk %"PRId64" has start position %"
            PRId64". Perhaps the BAM and REF are mismatched?",
                    bamChunk->refSeqName, fullRefLen, chunkIdx, bamChunk->chunkBoundaryStart);
        }
        RleString *rleReference = bamChunk_getReferenceSubstring(bamChunk, referenceSequences, params);
        st_logInfo(">%s Going to process a chunk for reference sequence: %s, starting at: %i and ending at: %i\n",
                   logIdentifier, bamChunk->refSeqName, (int) bamChunk->chunkBoundaryStart,
                   (int) (fullRefLen < bamChunk->chunkBoundaryEnd ? fullRefLen : bamChunk->chunkBoundaryEnd));

        // Convert bam lines into corresponding reads and alignments
        st_logInfo(">%s Parsing input reads from file: %s\n", logIdentifier, bamInFile);
        stList *reads = stList_construct3(0, (void (*)(void *)) bamChunkRead_destruct);
        stList *alignments = stList_construct3(0, (void (*)(void *)) stList_destruct);
        convertToReadsAndAlignments(bamChunk, rleReference, reads, alignments);

        // do downsampling if appropriate
        if (params->polishParams->maxDepth > 0) {
            // get downsampling structures
            stList *filteredReads = stList_construct3(0, (void (*)(void *)) bamChunkRead_destruct);
            stList *discardedReads = stList_construct3(0, (void (*)(void *)) bamChunkRead_destruct);
            stList *filteredAlignments = stList_construct3(0, (void (*)(void *)) stList_destruct);
            stList *discardedAlignments = stList_construct3(0, (void (*)(void *)) stList_destruct);

            bool didDownsample = poorMansDownsample(params->polishParams->maxDepth, bamChunk, reads, alignments,
                    filteredReads, filteredAlignments, discardedReads, discardedAlignments);

            // we need to destroy the discarded reads and structures
            if (didDownsample) {
                st_logInfo(" %s Downsampled from %"PRId64" to %"PRId64" reads\n", logIdentifier,
                        stList_length(reads), stList_length(filteredReads));
                // free all reads and alignments not used
                stList_destruct(discardedReads);
                stList_destruct(discardedAlignments);
                // still has all the old reads, need to not free these
                stList_setDestructor(reads, NULL);
                stList_setDestructor(alignments, NULL);
                stList_destruct(reads);
                stList_destruct(alignments);
                // and keep the filtered reads
                reads = filteredReads;
                alignments = filteredAlignments;
            }
            // no downsampling, we just need to free the (empty) objects
            else {
                stList_destruct(filteredReads);
                stList_destruct(filteredAlignments);
                stList_destruct(discardedReads);
                stList_destruct(discardedAlignments);
            }
        }

        // prep for ploishing
        Poa *poa = NULL; // The poa alignment
        char *polishedConsensusString = NULL; // The polished reference string

        // Run the polishing method
        int64_t totalNucleotides = 0;
        if (st_getLogLevel() >= info) {
            for (int64_t u = 0 ; u < stList_length(reads); u++) {
                totalNucleotides += strlen(((BamChunkRead*)stList_get(reads, u))->rleRead->rleString);
            }
            st_logInfo(">%s Running polishing algorithm with %"PRId64" reads and %"PRIu64"K nucleotides\n",
                       logIdentifier, stList_length(reads), totalNucleotides >> 10);
        }

        // Generate partial order alignment (POA) (destroys rleAlignments in the process)
        poa = poa_realignAll(reads, alignments, rleReference, params->polishParams);

        // Log info about the POA
        if (st_getLogLevel() >= info) {
            st_logInfo(">%s Summary stats for POA:\t", logIdentifier);
            poa_printSummaryStats(poa, stderr);
        }
        if (st_getLogLevel() >= debug) {
            poa_print(poa, stderr, reads, 5);
        }

        // Write any optional outputs about repeat count and POA, etc.
        if(outputPoaDotBase != NULL) {
            char *outputPoaDotFilename = stString_print("%s.poa.C%05"PRId64".%s-%"PRId64"-%"PRId64".dot",
                                                        outputPoaDotBase, chunkIdx, bamChunk->refSeqName,
                                                        bamChunk->chunkBoundaryStart, bamChunk->chunkBoundaryEnd);
            FILE *outputPoaTsvFileHandle = fopen(outputPoaDotFilename, "w");
            poa_printDOT(poa, outputPoaTsvFileHandle, reads);
            fclose(outputPoaTsvFileHandle);
            free(outputPoaDotFilename);
        }
        if(outputPoaTsvBase != NULL) {
            char *outputPoaTsvFilename = stString_print("%s.poa.C%05"PRId64".%s-%"PRId64"-%"PRId64".tsv",
                                                        outputPoaTsvBase, chunkIdx, bamChunk->refSeqName,
                                                        bamChunk->chunkBoundaryStart, bamChunk->chunkBoundaryEnd);
            FILE *outputPoaTsvFileHandle = fopen(outputPoaTsvFilename, "w");
            poa_printCSV(poa, outputPoaTsvFileHandle, reads, params->polishParams->repeatSubMatrix, 5);
            fclose(outputPoaTsvFileHandle);
            free(outputPoaTsvFilename);
        }
        if(outputRepeatCountBase != NULL) {
            char *outputRepeatCountFilename = stString_print("%s.repeatCount.C%05"PRId64".%s-%"PRId64"-%"PRId64".tsv",
                                                             outputRepeatCountBase, chunkIdx, bamChunk->refSeqName,
                                                             bamChunk->chunkBoundaryStart, bamChunk->chunkBoundaryEnd);
            FILE *outputRepeatCountFileHandle = fopen(outputRepeatCountFilename, "w");
            poa_printRepeatCountsCSV(poa, outputRepeatCountFileHandle, reads);
            fclose(outputRepeatCountFileHandle);
            free(outputRepeatCountFilename);
        }


        // handle diploid case
        if(diploid) {
            // Get the bubble graph representation
            //todo this hack needs to be fixed
            bool useReadAlleles = params->polishParams->useReadAlleles;
            params->polishParams->useReadAlleles = params->polishParams->useReadAllelesInPhasing;
            BubbleGraph *bg = bubbleGraph_constructFromPoa(poa, reads, params->polishParams);
            params->polishParams->useReadAlleles = useReadAlleles;

            // Now make a POA for each of the haplotypes
            stHash *readsToPSeqs;
            stGenomeFragment *gf = bubbleGraph_phaseBubbleGraph(bg, bamChunk->refSeqName, reads, params, &readsToPSeqs);

            stSet *readsBelongingToHap1, *readsBelongingToHap2;
            stGenomeFragment_phaseBamChunkReads(gf, readsToPSeqs, reads, &readsBelongingToHap1, &readsBelongingToHap2);
            st_logInfo(" %s After phasing, of %i reads got %i reads partitioned into hap1 and %i reads partitioned "
                       "into hap2 (%i unphased)\n", logIdentifier, (int)stList_length(reads),
                       (int)stSet_size(readsBelongingToHap1), (int)stSet_size(readsBelongingToHap2),
                       (int)(stList_length(reads) - stSet_size(readsBelongingToHap1)- stSet_size(readsBelongingToHap2)));

            // Debug report of hets
            if (st_getLogLevel() <= info) {
                uint64_t totalHets = 0;
                for (uint64_t h = 0; h < gf->length; h++) {
                    Bubble *b = &bg->bubbles[h + gf->refStart];
                    if (gf->haplotypeString1[h] != gf->haplotypeString2[h]) {
                        st_logDebug(" %s Got predicted het at bubble %i %s %s\n", logIdentifier, (int) h + gf->refStart,
                                    b->alleles[gf->haplotypeString1[h]]->rleString,
                                    b->alleles[gf->haplotypeString2[h]]->rleString);
                        totalHets++;
                    } else if (!rleString_eq(b->alleles[gf->haplotypeString1[h]], b->refAllele)) {
                        st_logDebug(" %s Got predicted hom alt at bubble %i %i\n", logIdentifier, (int) h + gf->refStart,
                                    (int) gf->haplotypeString1[h]);
                    }
                }
                st_logInfo(" %s In phasing chunk, got: %i hets from: %i total sites (fraction: %f)\n", logIdentifier,
                        (int) totalHets, (int) gf->length, (float) totalHets / gf->length);
            }

            st_logInfo(" %s Building POA for each haplotype\n", logIdentifier);
            uint64_t *hap1 = getPaddedHaplotypeString(gf->haplotypeString1, gf, bg, params);
            uint64_t *hap2 = getPaddedHaplotypeString(gf->haplotypeString2, gf, bg, params);

            Poa *poa_hap1 = bubbleGraph_getNewPoa(bg, hap1, poa, reads, params);
            Poa *poa_hap2 = bubbleGraph_getNewPoa(bg, hap2, poa, reads, params);

            if(params->polishParams->useRunLengthEncoding) {
                st_logInfo(" %s Using read phasing to reestimate repeat counts in phased manner\n", logIdentifier);
                poa_estimatePhasedRepeatCountsUsingBayesianModel(poa_hap1, reads, params->polishParams->repeatSubMatrix,
                                                                 readsBelongingToHap1, readsBelongingToHap2, params->polishParams);
                poa_estimatePhasedRepeatCountsUsingBayesianModel(poa_hap2, reads, params->polishParams->repeatSubMatrix,
                                                                 readsBelongingToHap2, readsBelongingToHap1, params->polishParams);
            }

            // output
            RleString *polishedRleConsensusH1 = rleString_copy(poa_hap1->refString);
            RleString *polishedRleConsensusH2 = rleString_copy(poa_hap2->refString);
            char *polishedConsensusStringH1 = rleString_expand(polishedRleConsensusH1);
            char *polishedConsensusStringH2 = rleString_expand(polishedRleConsensusH2);

            chunkResultsH1[chunkIdx] = polishedConsensusStringH1;
            chunkResultsH2[chunkIdx] = polishedConsensusStringH2;
            readSetsH1[chunkIdx] = readsBelongingToHap1;
            readSetsH2[chunkIdx] = readsBelongingToHap2;

            //ancillary files
            if (outputHaplotypeBamBase != NULL || outputHaplotypeReadsBase != NULL) {
                writeHaplotypedOutput(bamChunk, bamInFile, outputHaplotypeBamBase, outputHaplotypeReadsBase,
                        readsBelongingToHap1, readsBelongingToHap2, logIdentifier);
            }

            // helen
            #ifdef _HDF5
            if (helenFeatureType != HFEAT_NONE) {
                handleDiploidHelenFeatures(helenFeatureType, trueReferenceBamChunker, splitWeightMaxRunLength,
                        helenHDF5Files, fullFeatureOutput, trueReferenceBam, trueReferenceBamHap2, params,
                        logIdentifier, chunkIdx, bamChunk, reads, poa_hap1, poa_hap2, readsBelongingToHap1,
                        readsBelongingToHap2, polishedConsensusStringH1, polishedConsensusStringH2,
                        polishedRleConsensusH1, polishedRleConsensusH2);
            }
            #endif

            // Cleanup
            free(hap1);
            free(hap2);
            rleString_destruct(polishedRleConsensusH1);
            rleString_destruct(polishedRleConsensusH2);
            bubbleGraph_destruct(bg);
            stGenomeFragment_destruct(gf);
            poa_destruct(poa_hap1);
            poa_destruct(poa_hap2);
            stHash_destruct(readsToPSeqs);

        } else {

            // get polished reference string and expand RLE (regardless of whether RLE was applied)
            if (params->polishParams->useRunLengthEncoding) {
                poa_estimateRepeatCountsUsingBayesianModel(poa, reads, params->polishParams->repeatSubMatrix);
            }
            RleString *polishedRleConsensus = poa->refString;
            polishedConsensusString = rleString_expand(polishedRleConsensus);

            // save polished reference string to chunk output array
            chunkResults[chunkIdx] = polishedConsensusString;

            // HELEN feature outputs
            #ifdef _HDF5
            if (helenFeatureType != HFEAT_NONE) {
                handleHelenFeatures(helenFeatureType, trueReferenceBamChunker, splitWeightMaxRunLength,
                                    helenHDF5Files, fullFeatureOutput, trueReferenceBam, params, logIdentifier, chunkIdx,
                                    bamChunk, poa, reads, polishedConsensusString, polishedRleConsensus);

            }
            #endif
        }

        // report timing
        if (st_getLogLevel() >= info) {
            st_logInfo(">%s Chunk with %"PRId64" reads and %"PRIu64"K nucleotides processed in %d sec\n",
                       logIdentifier, stList_length(reads), totalNucleotides >> 10,
                       (int) (time(NULL) - chunkStartTime));
        }

        // Cleanup
        rleString_destruct(rleReference);
        poa_destruct(poa);
        stList_destruct(reads);
        stList_destruct(alignments);
        free(logIdentifier);
    }

    // merge chunks
    if (diploid) {
        handleDiploidMerge(bamChunker, chunkResultsH1, chunkResultsH2, readSetsH1, readSetsH2, numThreads,
                params, polishedReferenceOutFh, polishedReferenceOutFhH2);
    } else {
        handleMerge(bamChunker, chunkResults, numThreads, params, polishedReferenceOutFh);
    }

    // everything has been written, cleanup merging infrastructure
    fclose(polishedReferenceOutFh);
    if (polishedReferenceOutFhH2 != NULL) fclose(polishedReferenceOutFhH2);
    for (int64_t chunkIdx = 0; chunkIdx < bamChunker->chunkCount; chunkIdx++) {
        free(chunkResults[chunkIdx]);
        if (diploid) {
            free(chunkResultsH2[chunkIdx]);
            stSet_destruct(readSetsH1[chunkIdx]);
            stSet_destruct(readSetsH2[chunkIdx]);
        }
    }

    // Cleanup
    bamChunker_destruct(bamChunker);
    stHash_destruct(referenceSequences);
    params_destruct(params);
    if (trueReferenceBam != NULL) free(trueReferenceBam);
    if (trueReferenceBamChunker != NULL) bamChunker_destruct(trueReferenceBamChunker);
    if (regionStr != NULL) free(regionStr);
    #ifdef _HDF5
    if (helenHDF5Files != NULL) {
        for (int64_t i = 0; i < numThreads; i++) {
            HelenFeatureHDF5FileInfo_destruct((HelenFeatureHDF5FileInfo *) helenHDF5Files[i]);
        }
        free(helenHDF5Files);
    }
    #endif
    stList_destruct(chunkOrder);
    free(chunkResults);
    if (diploid) {
        free(chunkResultsH2);
        free(readSetsH1);
        free(readSetsH2);
    }
    free(outputBase);
    free(bamInFile);
    free(referenceFastaFile);
    free(paramsFile);

    // log completion
    char *timeDescriptor = getTimeDescriptorFromSeconds(time(NULL) - startTime);
    st_logCritical("> Finished polishing in %s.\n", timeDescriptor);
    free(timeDescriptor);

//    while(1); // Use this for testing for memory leaks

    return 0;
}

