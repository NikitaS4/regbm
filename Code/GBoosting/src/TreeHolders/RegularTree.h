#ifndef REGULAR_TREE_INCLUDED
#define REGULAR_TREE_INCLUDED

#include "TreeHolder.h"
#include <vector>
#include <functional>


class RegularTree: public TreeHolder {
public:
    RegularTree(const size_t treeDepth, const size_t featureCnt,
        const size_t threadCnt);
    virtual ~RegularTree();

    virtual void newTree(const size_t* features, const FVal_t* thresholds,
        const Lab_t* leaves) final override;
    virtual void popTree() final override;

    virtual Lab_t predictTree(const pytensor1& sample, const size_t treeNum) const final override;
    virtual void predictTreeFit(const pytensor2& xTrain, const pytensor2& xValid,
        const size_t treeNum, pytensorY& residuals, pytensorY& preds,
        pytensorY& validRes, pytensorY& validPreds) const final override;
    virtual Lab_t predictAllTrees(const pytensor1& sample) const final override;
    virtual pytensorY predictAllTrees2d(const pytensor2& sample) const final override;
    virtual Lab_t predictFromTo(const pytensor1& sample, const size_t from, 
        const size_t to) const final override;

    virtual pytensorY predictTree2d(const pytensor2& xPred, const size_t treeNum) const final override;
    virtual std::string serialize(const char delimeter, const Lab_t zeroPredictor) const final override;
    
    static RegularTree* parse(const char* repr,
        const std::vector<size_t> delimPos,
        const size_t delimStart, const size_t featureCnt,
        const size_t treeCnt, const size_t treeDepth,
        const size_t threadCnt);

private:
    // fields
    std::vector<size_t*> features;
    std::vector<FVal_t*> thresholds;
    std::vector<Lab_t*> leaves;

    // methods
    inline void validateFeatures();
    inline void validateTreeNum(const size_t treeNum) const;

    pytensorY predictTree2dMutlithreaded(const pytensor2& xPred,
        const size_t treeNum) const;

    pytensorY predictTree2dSingleThread(const pytensor2& xPred,
        const size_t treeNum) const;

    inline void predictFitMultithreaded(const pytensor2& xTrain, const pytensor2& xValid,
        const size_t treeNum, pytensorY& residuals, pytensorY& preds,
        pytensorY& validRes, pytensorY& validPreds) const;

    pytensorY allTrees2dMultithreaded(const pytensor2& xPred) const;

    pytensorY predict2dProxy(const pytensor2& xPred,
        const bool allTrees, const size_t treeNum) const;

    std::function<void()> getCallback(const size_t bias,
        const size_t batchSize, const size_t treeNum,
        const pytensor2& xPred, size_t& semThreadsFinish,
        pytensorY& answers) const;

    std::function<void()> getCallbackAll(const size_t bias,
        const size_t batchSize, const size_t treeNum,
        const pytensor2& xPred, size_t& semThreadsFinish,
        pytensorY& answers) const;

    // constants
    static const int busyWaitMs = 1;
};

#endif // REGULAR_TREE_INCLUDED
