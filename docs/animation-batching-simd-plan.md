# Animation System: Batching + SIMD Opportunities

This note identifies where RetroFE’s current animation path can benefit from batching and SIMD, what data layout changes would help most, and whether each change is likely worth implementing.

## Current bottlenecks in the code path

1. **Per-component, per-tween virtual-ish workflow with pointer chasing**  
   `Component::animate()` iterates each tween in the current set and reads it through `TweenSet::getTween(i)` (`unique_ptr<Tween>` under the hood), then dispatches through a large `switch` on property and calls `Tween::animate(...)`. This creates branch-heavy code and poor cache locality.  
   Relevant code: `Component::animate`, `TweenSet`, `Tween::animateSingle`.

2. **AoS/object layout limits vectorization**  
   Tweens are stored as heap objects (`std::unique_ptr<Tween>`), and animations are nested containers of `shared_ptr<TweenSet>`. This is flexible but not SIMD-friendly and expensive to traverse at scale.

3. **Repeated playlist filter parsing in hot loop**  
   For every tween each update, code can build a `stringstream` and split comma-separated filters. That is substantial overhead relative to easing math.

4. **Multiple branches per animated property**  
   For each tween there is an inner `switch` for property assignment and a branch on `startDefined`, plus algorithm branching in `Tween::animateSingle`.

## Where batching should be introduced first (highest ROI)

## 1) Batch *evaluation* in `Component::animate()` (low-risk, immediate gains)

### Change
Introduce a precompiled runtime tween representation for each `TweenSet` that removes string parsing and minimizes branching:

- Build once when loading XML / creating animations:
  - `property` as compact enum
  - `algorithm` as compact enum
  - `startDefined` + resolved `start`
  - `end`, `duration`, `invDuration`
  - parsed playlist filter mask/list (no per-frame stringstream)
- At runtime, evaluate these precompiled entries with a tight loop.

### Data structure
Use **SoA-like grouped arrays per algorithm** inside a compiled tween set:

- `std::vector<float> start, end, change, duration, invDuration`
- `std::vector<uint8_t> property`
- `std::vector<uint8_t> flags` (`startDefined`, etc.)
- Optionally maintain groups per algorithm (`linearGroup`, `easeInQuadGroup`, ...).

This lets you run a contiguous loop per algorithm, opening a straightforward SIMD path.

### Why here
This is exactly where per-frame cost accumulates and where data is currently most fragmented.

---

## 2) Batch *property writes* by mapping tween properties to offsets (moderate risk)

### Change
Replace the giant property `switch` in `Component::animate()` with a small property table:

- For float properties, map `TweenProperty -> offsetof(ViewInfo, field)`.
- For integer-like properties (`Layer`, `Monitor`), keep separate conversion handlers.

### Data structure

- `constexpr` table of metadata per property:
  - destination kind (`float`/`uint`/special)
  - offset
  - clamp/cast policy

### Why
Cuts branch pressure and keeps hot loop more vectorization-friendly.

---

## 3) Batch across components in `Page::update()` (moderate-high effort)

### Change
Add an animation scheduler that gathers active components into work buckets each frame:

- Bucket key: `(algorithm, propertyKind, monitor?)`
- Evaluate bucket arrays in one pass, then scatter results back to components.

### Data structure
A frame-local job buffer (SoA):

- `componentPtr[]`
- `propertyId[]`
- `elapsed[]`
- `start[]`, `change[]`, `invDuration[]`
- grouped/radix-partitioned by algorithm

### Why
Enables wider SIMD batches and improves threading opportunities later.

## SIMD strategy (practical for this codebase)

## Stage A: auto-vectorization first

- Keep easing kernels as small `inline` free functions over arrays.
- Use SoA and contiguous loops; compile with high optimization (`-O3`) and target-specific flags.
- This often gives “free” SSE/AVX/NEON wins without architecture-specific code.

## Stage B: explicit SIMD only for top algorithms

Implement explicit SIMD kernels for high-frequency easings first:

- `linear`
- `easeIn/Out/InOutQuadratic`
- `easeIn/Out/InOutCubic`

Leave transcendental-heavy easings (`sine`, `exponential`, `circular`) scalar initially unless profiling proves they dominate.

## Stage C: optional approximate math

If needed, add optional fast approximations for `sin/cos/pow/sqrt` behind a config flag (`fastAnimationMath=true`) with acceptable visual tolerance.

## Recommended migration plan

1. **Instrument before/after**
   - Add timing counters around `Page::update()`, `Component::animate()`, and tween count processed.
2. **Compile tween sets** (no behavior change)
   - Pre-parse playlist filters.
   - Cache `invDuration` and `change`.
3. **Swap hot loop to compiled representation**
   - Keep scalar math first, verify identical output.
4. **Algorithm-grouped loops + auto-vectorization**
   - Measure gains.
5. **Optional explicit SIMD kernels** for top 2–4 easing families.
6. **Only then consider cross-component frame scheduler**.

## Worthwhile? Expected payoff

- **Yes, worthwhile** if typical layouts animate many components simultaneously (menus + media + text effects).
- Biggest likely wins:
  1. removing per-frame playlist filter string parsing,
  2. eliminating pointer-heavy tween traversal,
  3. improving cache locality with SoA,
  4. reducing switch/branch density.
- SIMD alone without data-layout cleanup will likely underperform expectations.

## What *not* to do first

- Don’t start with architecture-specific intrinsics before SoA + profiling.
- Don’t parallelize tiny per-component loops prematurely; synchronization overhead can erase gains.
- Don’t convert every easing to approximations unless profiling proves transcendental math is a bottleneck.

## Concrete “best insertion points” in current source

- **Primary hotspot:** `RetroFE/Source/Graphics/Component/Component.cpp` (`Component::animate`)  
  Introduce compiled tween buffers and property metadata here first.
- **Model/storage layer:** `RetroFE/Source/Graphics/Animate/TweenSet.*`, `Animation.*`  
  Add compiled/packed storage alongside existing objects, then phase out pointer-based traversal.
- **Frame-level batching point:** `RetroFE/Source/Graphics/Page.cpp` (`Page::update`)  
  Add optional scheduler once single-component path is optimized and benchmarked.
- **Kernel math location:** `RetroFE/Source/Graphics/Animate/Tween.*`  
  Split scalar kernels from dispatch; add batched evaluation entry points.

## Suggested target data model (end state)

- `CompiledTweenSet`
  - `AlgorithmGroup groups[NUM_ALGOS]`
- `AlgorithmGroup`
  - SoA arrays: `start[]`, `change[]`, `invDuration[]`, `duration[]`, `elapsedClamp[]`
  - `property[]`, `componentIndex[]`, `flags[]`
- optional `FrameAnimationBatch`
  - transient per-frame grouped views for cross-component processing

This end state keeps authoring flexibility while making runtime animation updates data-oriented and SIMD-ready.
