#include "RegularTree.h"
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <math.h>


RegularTree::RegularTree(const size_t treeDepth,
    const size_t featureCnt, const size_t threadCnt):
    TreeHolder(treeDepth, featureCnt, threadCnt) {
    // ctor
}


RegularTree::~RegularTree() {
    // dtor
    for (size_t i = 0; i < treeCnt; ++i) {
        delete [] features[i];
        delete [] thresholds[i];
        delete [] leaves[i];

        features[i] = nullptr;
        thresholds[i] = nullptr;
        leaves[i] = nullptr;
    }
}


void RegularTree::newTree(const size_t* features, const FVal_t* thresholds,
    const Lab_t* leaves) {
    static const size_t featuresSize = treeDepth * sizeof(*features);
    static const size_t thresholdSize = innerNodes * sizeof(*thresholds);
    static const size_t leavesSize = leafCnt * sizeof(*leaves);
    ++treeCnt;
    // copy arrays
    this->features.push_back((size_t*)(std::memcpy(new size_t[treeDepth], features, featuresSize)));
    this->thresholds.push_back((FVal_t*)(std::memcpy(new FVal_t[innerNodes], thresholds, thresholdSize)));
    this->leaves.push_back((Lab_t*)(std::memcpy(new Lab_t[leafCnt], leaves, leavesSize)));

    // this will fix errors
    // TODO: find out the reason for the wrong values
    validateFeatures();
}


void RegularTree::popTree() {
    // decrease tree count
    --treeCnt;

    // free memory
    delete [] features[treeCnt];
    delete [] thresholds[treeCnt];
    delete [] leaves[treeCnt];

    // assign nulls to pointers
    features[treeCnt] = nullptr;
    thresholds[treeCnt] = nullptr;
    leaves[treeCnt] = nullptr;

    // pop vectors
    features.pop_back();
    thresholds.pop_back();
    leaves.pop_back();
}


Lab_t RegularTree::predictTree(const pytensor1& sample, 
    const size_t treeNum) const {
    validateTreeNum(treeNum);
    // get pointers for faster access
    const size_t* curFeatures = features[treeNum];
    const FVal_t* curThresholds = thresholds[treeNum];

    // TODO: use getCallback(...)
    size_t curNode = 0;
    // tree traverse
    for (size_t h = 0; h < treeDepth; ++h) {
        if (sample(curFeatures[h]) < curThresholds[curNode])
            curNode = 2 * curNode + 1;
        else
            curNode = 2 * curNode + 2;
    }
    return leaves[treeNum][curNode - innerNodes];
}


void RegularTree::predictTreeFit(const pytensor2& xTrain, const pytensor2& xValid,
        const size_t treeNum, pytensorY& residuals, pytensorY& preds,
        pytensorY& validRes, pytensorY& validPreds) const {
    if (threadCnt > 1) {
        predictFitMultithreaded(xTrain, xValid, treeNum,
            residuals, preds, validRes, validPreds);
    } else {
        // predict on train subset
        pytensorY curPred = predictTree2dSingleThread(xTrain, treeNum);
        preds += curPred;
        residuals -= curPred;
        // predict on test subset
        curPred = predictTree2dSingleThread(xValid, treeNum);
        validPreds += curPred;
        validRes -= curPred;
    }
}


Lab_t RegularTree::predictAllTrees(const pytensor1& sample) const {
    Lab_t curSum = 0;
    for (size_t i = 0; i < treeCnt; ++i)
        curSum += predictTree(sample, i);
    return curSum;
}


pytensorY RegularTree::predictAllTrees2d(const pytensor2& sample) const {
    if (threadCnt == 1) {
        pytensorY answers = xt::zeros<Lab_t>({sample.shape(0)});
        for (size_t i = 0; i < treeCnt; ++i) {
            answers += predictTree2dSingleThread(sample, i);
        }
        return answers;
    } else {
        return allTrees2dMultithreaded(sample);
    }
}


Lab_t RegularTree::predictFromTo(const pytensor1& sample, const size_t from,
    const size_t to) const {
    Lab_t curSum = 0;
    for (size_t i = from; i < to; ++i)
        curSum += predictTree(sample, i);
    return curSum;
}


pytensorY RegularTree::predictTree2d(const pytensor2& xPred,
    const size_t treeNum) const {
    validateTreeNum(treeNum);
    if (threadCnt > 1)
        return predictTree2dMutlithreaded(xPred, treeNum);
    else
        return predictTree2dSingleThread(xPred, treeNum);
}


void RegularTree::validateFeatures() {
    for (auto & curFeatureArr: features) {
        for (size_t h = 0; h < treeDepth; ++h) {
            if (curFeatureArr[h] >= featureCnt)
                // too big value
                curFeatureArr[h] = featureCnt - 1;
        }
    }
}


void RegularTree::validateTreeNum(const size_t treeNum) const {
    if (treeNum >= features.size())
        throw std::runtime_error("wrong treeNum");
    if (treeNum >= thresholds.size())
        throw std::runtime_error("wrong treeNum");
    if (treeNum >= leaves.size())
        throw std::runtime_error("wrong treeNum");
}


pytensorY RegularTree::predictTree2dSingleThread(const pytensor2& xPred,
    const size_t treeNum) const {
    // get pointers for faster access
    const size_t* curFeatures = features[treeNum];
    const FVal_t* curThresholds = thresholds[treeNum];
    const size_t sampleCnt = xPred.shape(0);

    // tensor to store and return predictions
    pytensorY answers = xt::zeros<Lab_t>({sampleCnt});

    // use code defined in getCallback(...)
    size_t semFict = 1; // don't need to acqiure/release lock
    getCallback(0, sampleCnt, treeNum, xPred, semFict,
        answers)(); // get & launch
    return answers;
}


pytensorY RegularTree::predictTree2dMutlithreaded(const pytensor2& xPred,
    const size_t treeNum) const {
    static const bool allTrees = false;
    return predict2dProxy(xPred, allTrees, treeNum);
}


void RegularTree::predictFitMultithreaded(const pytensor2& xTrain, const pytensor2& xValid,
        const size_t treeNum, pytensorY& residuals, pytensorY& preds,
        pytensorY& validRes, pytensorY& validPreds) const {
    // part of threads -> train, part -> valid
    // compute the parts
    std::vector<std::thread> threads;
    const size_t trainLen = xTrain.shape(0);
    const size_t validLen = xValid.shape(0);
    const size_t commonLen = trainLen + validLen;
    size_t threadsForValid = (size_t)(floor((threadCnt * (double)(validLen) / (double)(commonLen))));
    // max(threadsForValid, 1)
    // at least one thread
    threadsForValid = (threadsForValid > 1)? (threadsForValid) : (1);
    size_t threadsForTrain = threadCnt - threadsForValid;

    // semaphore (to wait until threads finish)
    size_t semThreadsFinish = threadCnt;
    const size_t semUnlocked = 0;

    // prepare train threads
    // each thread will predict on it's own batch
    size_t bias; // bias of the batch
    size_t batchSize = trainLen / threadsForTrain;
    // the size of the last batch (it may differ)
    size_t lastBatchSize = trainLen - (threadsForTrain - 1) * batchSize;
    // tensor to store and return predictions
    pytensorY trainPreds = xt::zeros<Lab_t>({trainLen});
    for (size_t i = 0; i < threadsForTrain - 1; ++i) {
        bias = i * batchSize; // compute batch bias
        threads.push_back(std::thread(getCallback(bias, batchSize,
            treeNum, xTrain, semThreadsFinish, trainPreds)));
        threads[i].detach(); // launch the thread
    }
    // predict on the last train batch
    bias = (threadsForTrain - 1) * batchSize;
    auto lastThread = getCallback(bias, batchSize, treeNum,
        xTrain, semThreadsFinish, trainPreds);
    threads.push_back(std::thread(lastThread));
    threads[threadsForTrain - 1].detach();

    // prepare validation threads
    // each thread will predict on it's own batch
    size_t biasValid; // bias of the batch
    size_t batchSizeValid = validLen / threadsForValid;
    // the size of the last batch (it may differ)
    size_t lastBatchSizeVal = validLen - (threadsForValid - 1) * batchSizeValid;
    // tensor to store and return predictions
    pytensorY predsForValid = xt::zeros<Lab_t>({validLen});
    for (size_t i = 0; i < threadsForValid - 1; ++i) {
        biasValid = i * batchSizeValid; // compute batch bias
        threads.push_back(std::thread(getCallback(biasValid, batchSizeValid,
            treeNum, xValid, semThreadsFinish, predsForValid)));
        threads[i + threadsForTrain].detach(); // launch the thread
    }
    // predict on the last valid batch
    // we will process the last batch in this thread (main)
    biasValid = (threadsForValid - 1) * batchSizeValid;
    getCallback(biasValid, lastBatchSizeVal, treeNum, xValid,
        semThreadsFinish, predsForValid)(); // get & launch

    while (semThreadsFinish > semUnlocked) {
        // busy wait (until threads finishes predictions)
        std::this_thread::sleep_for(std::chrono::milliseconds(busyWaitMs));
    }
    // now all predictions have been aggregated, can update residuals and predictions

    // one thread for train, one for validation
    // validation will be updated in the main thread
    semThreadsFinish = 1;
    // train
    std::thread trainThread = std::thread([&]() {
        preds += trainPreds;
        residuals -= trainPreds;
        semThreadsFinish -= 1;
    });
    trainThread.detach();
    // validation
    validPreds += predsForValid;
    validRes -= predsForValid;
    
    while (semThreadsFinish > semUnlocked) {
        // busy wait (until train updates finish)
        std::this_thread::sleep_for(std::chrono::milliseconds(busyWaitMs));
    }
    // now all updates performed
}


pytensorY RegularTree::allTrees2dMultithreaded(const pytensor2& xPred) const {
    static const bool allTrees = true;
    static const size_t treeNumStub = 0;
    return predict2dProxy(xPred, allTrees, treeNumStub);
}


pytensorY RegularTree::predict2dProxy(const pytensor2& xPred,
        const bool allTrees, const size_t treeNum) const {
    // decide which callback to get
    std::function<void()> (RegularTree::*callbackGetter)(const size_t bias,
        const size_t batchSize, const size_t treeNum,
        const pytensor2& xPred, size_t& semThreadsFinish,
        pytensorY& answers) const;
    if (allTrees) {
        callbackGetter = &RegularTree::getCallbackAll;
    }
    else {
        callbackGetter = &RegularTree::getCallback;
    }

    // predict
    std::vector<std::thread> threads;
    // init "semaphore" with the max value
    // each thread will decrement the semaphore before finish
    size_t semThreadsFinish = threadCnt;
    static const size_t semUnlocked = 0;

    // each thread will predict on it's own batch
    size_t bias; // bias of the batch
    size_t batchSize = xPred.shape(0) / threadCnt;
    // the size of the last batch (it may differ)
    size_t lastBatchSize = xPred.shape(0) - (threadCnt - 1) * batchSize;
    // tensor to store and return predictions
    pytensorY answers = xt::zeros<Lab_t>({xPred.shape(0)});
    // create threads for prediction
    for (size_t i = 0; i < threadCnt - 1; ++i) {
        bias = i * batchSize; // compute batch bias
        // each thread has it's own unique callback
        // (to avoid data races)
        threads.push_back(std::thread((this->*callbackGetter)(bias, batchSize,
            treeNum, xPred, semThreadsFinish, answers)));
        threads[i].detach(); // launch the thread
    }
    // the last batch will be processed in this thread (main)
    bias = (threadCnt - 1) * batchSize;
    (this->*callbackGetter)(bias, lastBatchSize, treeNum, xPred,
        semThreadsFinish, answers)(); // get & launch

    while (semThreadsFinish > semUnlocked) {
        // busy wait (until threads finishes predictions)
        std::this_thread::sleep_for(std::chrono::milliseconds(busyWaitMs));
    }
    // now all predictions have been aggregated, can return an answer

    return answers;
}


std::function<void()> RegularTree::getCallback(const size_t bias,
    const size_t batchSize, const size_t treeNum,
    const pytensor2& xPred, size_t& semThreadsFinish,
    pytensorY& answers) const {
    // get pointers for faster access
    const size_t* curFeatures = features[treeNum];
    const FVal_t* curThresholds = thresholds[treeNum];
    const Lab_t* curLeaves = leaves[treeNum];
    // pass by values
    const size_t treeDepth = this->treeDepth;
    const size_t innerNodes = this->innerNodes;
    // don't pass 'this' by reference, don't pass 'this' at all
    // this is needed to avoid data races
    return [curFeatures, curThresholds, curLeaves, treeDepth,
        innerNodes, bias, batchSize, &xPred, &semThreadsFinish,
        &answers]() mutable {
        // compute the end of the batch
        const size_t upperLimit = batchSize + bias;
        size_t curNode = 0; // current node in the decision tree
        // predict for all batch members
        for (size_t j = bias; j < upperLimit; ++j) {
            // decision tree traverse
            for (size_t h = 0; h < treeDepth; ++h) {
                if (xPred(j, curFeatures[h]) < curThresholds[curNode])
                    curNode = 2 * curNode + 1;
                else
                    curNode = 2 * curNode + 2;
            }
            answers(j) = curLeaves[curNode - innerNodes];
            // remember to set curNode to 0 before the next step
            curNode = 0;
        }
        // thread is finishing, release "semaphore"
        semThreadsFinish -= 1;
    };
}


std::function<void()> RegularTree::getCallbackAll(const size_t bias,
        const size_t batchSize, const size_t treeNum,
        const pytensor2& xPred, size_t& semThreadsFinish,
        pytensorY& answers) const {
    // ignore treeNum
    // pass by values
    const size_t treeDepth = this->treeDepth;
    const size_t innerNodes = this->innerNodes;
    const std::vector<size_t*>& features = this->features;
    const std::vector<FVal_t*>& thresholds = this->thresholds;
    const std::vector<Lab_t*>& leaves = this->leaves;
    const size_t treeCnt = this->treeCnt;
    // don't pass 'this' by reference, don't pass 'this' at all
    // this is needed to avoid data races
    return [treeDepth, innerNodes, bias, batchSize, treeCnt,
        &xPred, &semThreadsFinish, &answers, &features,
        &thresholds, &leaves]() mutable {
        // compute the end of the batch
        const size_t upperLimit = batchSize + bias;
        size_t curNode = 0; // current node in the decision tree
        for (size_t tr = 0; tr < treeCnt; ++tr) {
            // predict for all trees
            // get pointers for faster access
            const size_t* curFeatures = features[tr];
            const FVal_t* curThresholds = thresholds[tr];
            const Lab_t* curLeaves = leaves[tr];
            // predict for all batch members
            for (size_t j = bias; j < upperLimit; ++j) {
                // decision tree traverse
                for (size_t h = 0; h < treeDepth; ++h) {
                    if (xPred(j, curFeatures[h]) < curThresholds[curNode])
                        curNode = 2 * curNode + 1;
                    else
                        curNode = 2 * curNode + 2;
                }
                answers(j) += curLeaves[curNode - innerNodes];
                // remember to set curNode to 0 before the next step
                curNode = 0;
            }
        }
        // thread is finishing, release "semaphore"
        semThreadsFinish -= 1;
    };
}