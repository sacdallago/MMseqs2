#include "Util.h"
#include "Parameters.h"
#include "Matcher.h"
#include "Debug.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "NucleotideMatrix.h"
#include "SubstitutionMatrix.h"
#include "Alignment.h"
#include "itoa.h"

#ifdef OPENMP
#include <omp.h>
#endif

int alignall(int argc, const char **argv, const Command &command) {
    Parameters &par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, false, 0, 0);

    DBReader<unsigned int> tdbr(par.db1.c_str(), par.db1Index.c_str(), par.threads, DBReader<unsigned int>::USE_DATA|DBReader<unsigned int>::USE_INDEX);
    tdbr.open(DBReader<unsigned int>::NOSORT);
    if (par.preloadMode != Parameters::PRELOAD_MODE_MMAP) {
        tdbr.readMmapedDataInMemory();
    }
    const int targetSeqType = tdbr.getDbtype();

    BaseMatrix *subMat;
    if (Parameters::isEqualDbtype(targetSeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
        subMat = new NucleotideMatrix(par.scoringMatrixFile.c_str(), 1.0, 0.0);
    } else {
        // keep score bias at 0.0 (improved ROC)
        subMat = new SubstitutionMatrix(par.scoringMatrixFile.c_str(), 2.0, 0.0);
    }

    DBReader<unsigned int> dbr_res(par.db2.c_str(), par.db2Index.c_str(), par.threads, DBReader<unsigned int>::USE_DATA|DBReader<unsigned int>::USE_INDEX);
    dbr_res.open(DBReader<unsigned int>::LINEAR_ACCCESS);

    DBWriter resultWriter(par.db3.c_str(), par.db3Index.c_str(), par.threads, par.compressed, Parameters::DBTYPE_PREFILTER_RES);
    resultWriter.open();

    EvalueComputation evaluer(tdbr.getAminoAcidDBSize(), subMat, par.gapOpen, par.gapExtend);
    const size_t flushSize = 100000000;
    size_t iterations = static_cast<int>(ceil(static_cast<double>(dbr_res.getSize()) / static_cast<double>(flushSize)));

    for (size_t i = 0; i < iterations; i++) {
        size_t start = (i * flushSize);
        size_t bucketSize = std::min(dbr_res.getSize() - (i * flushSize), flushSize);
        Debug::Progress progress(bucketSize);
#pragma omp parallel
        {
            unsigned int thread_idx = 0;
#ifdef OPENMP
            thread_idx = (unsigned int) omp_get_thread_num();
#endif

            Matcher matcher(targetSeqType, par.maxSeqLen, subMat, &evaluer, par.compBiasCorrection, par.gapOpen, par.gapExtend);

            Sequence query(par.maxSeqLen, targetSeqType, subMat, par.kmerSize, par.spacedKmer, par.compBiasCorrection);
            Sequence target(par.maxSeqLen, targetSeqType, subMat, par.kmerSize, par.spacedKmer, par.compBiasCorrection);

            char buffer[1024 + 32768];

            std::vector<unsigned int> results;
            results.reserve(300);

#pragma omp for schedule(dynamic, 1)
            for (size_t id = start; id < (start + bucketSize); id++) {
                progress.updateProgress();

                const unsigned int key = dbr_res.getDbKey(id);
                char *data = dbr_res.getData(id, thread_idx);

                results.clear();
                while (*data != '\0') {
                    Util::parseKey(data, buffer);
                    const unsigned int key = (unsigned int) strtoul(buffer, NULL, 10);
                    results.push_back(key);
                    data = Util::skipLine(data);
                }

                resultWriter.writeStart(thread_idx);
                for (size_t entryIdx1 = 0; entryIdx1 < results.size(); entryIdx1++) {
                    const unsigned int queryId = tdbr.getId(results[entryIdx1]);
                    const unsigned int queryKey = tdbr.getDbKey(queryId);
                    char *querySeq = tdbr.getData(queryId, thread_idx);
                    query.mapSequence(id, queryKey, querySeq);
                    matcher.initQuery(&query);

                    char * tmpBuff = Itoa::u32toa_sse2((uint32_t) queryKey, buffer);
                    *(tmpBuff-1) = '\t';
                    const unsigned int queryIdLen = tmpBuff - buffer;

                    for (size_t entryIdx = 0; entryIdx < results.size(); entryIdx++) {
                        const unsigned int targetId = tdbr.getId(results[entryIdx]);
                        const unsigned int targetKey = tdbr.getDbKey(targetId);
                        char *targetSeq = tdbr.getData(targetId, thread_idx);
                        target.mapSequence(id, targetKey, targetSeq);

                        if (Util::canBeCovered(par.covThr, par.covMode, query.L, target.L) == false) {
                            continue;
                        }

                        const bool isIdentity = (queryId == targetId && par.includeIdentity) ? true : false;
                        Matcher::result_t result = matcher.getSWResult(&target, INT_MAX, false, par.covMode, par.covThr, FLT_MAX,
                                                                       par.alignmentMode, par.seqIdMode, isIdentity);
                        // checkCriteria and Util::canBeCovered always work together
                        if (Alignment::checkCriteria(result, isIdentity, par.evalThr, par.seqIdThr, par.alnLenThr, par.covMode, par.covThr)) {
                            size_t len = Matcher::resultToBuffer(tmpBuff, result, true, false);
                            resultWriter.writeAdd(buffer, queryIdLen + len, thread_idx);
                        }
                    }
                }
                resultWriter.writeEnd(key, thread_idx);
            }
        }
        dbr_res.remapData();
    }
    resultWriter.close();
    dbr_res.close();
    delete subMat;
    tdbr.close();

    return EXIT_SUCCESS;
}


