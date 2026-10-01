// CoreML stub — Darling has no CoreML and Roblox links it non-weakly, so dyld refuses to start
// without it. Roblox imports five class symbols and nothing else (analysis/undefined.txt).
// Real ObjC classes, no methods: if Roblox ever actually messages one, the crash names the
// selector and we implement just that.
// ponytail: no methods, add one when a doesNotRecognizeSelector shows up in logs/.
@interface NSObject { void *isa; } @end   // matches the real root class layout; Foundation supplies it

#define STUB(name) @interface name : NSObject @end  @implementation name @end

STUB(MLModel)
STUB(MLModelConfiguration)
STUB(MLFeatureValue)
STUB(MLMultiArray)
STUB(MLPredictionOptions)
