# Uniformly-Partitioned Overlap-Save (UPOLS) with Frequency-Domain Delay Line

## Purpose

This document specifies the streaming UPOLS algorithm for real-time channel emulation on a USRP software-defined radio. The device receives a continuous complex-IQ sample stream at rate `fs`, convolves it with a channel impulse response `h` of up to `N = 4096` taps, and transmits the filtered stream with deterministic, low latency.

The algorithm partitions **both** the input signal and the channel impulse response into uniform blocks of size `B`. This is the (UP_S, UP_F) class in Wefers' taxonomy and corresponds to the standard UPOLS algorithm used by BruteFIR, Convolvotron, and similar real-time convolution engines.

## Revision history

**v2 changes vs. v1 (the original spec):**

1. **Complex-IQ output path fixed.** v1 extracted `real(y_buf[B..K-1])` — correct for audio, wrong for a USRP. IQ must remain complex.
2. **Latency model corrected.** v1 claimed latency = `B`. With the ping-pong architecture used on any realistic USRP driver (and by Armelloni 2003), algorithmic latency is `2B`. The `B`-sample figure assumes synchronous zero-time processing, which is not how UHD streaming works.
3. **TX timestamp scheduling fixed.** v1 used `rx_md.time_spec + B/fs` which schedules TX during the current receive block — a causality violation. The correct offset is at least `2B/fs` plus a safety margin.
4. **Overflow/underflow recovery added.** v1 had no handling; any RX overflow silently corrupts the FDL for the next `P` blocks.
5. **FDL zero-initialization made explicit.** v1's circular-buffer walkthrough was correct but depended on implicit zeroing.
6. **FFTW normalization moved out of hot loop** and into a pre-scaled `H_parts` (normalize once at init, not every block).
7. **Sample-rate range widened.** v1 quoted 1–50 MHz. USRP X410 supports up to 491.52 MSPS per channel; the relevant operating points for 5G NR are 61.44, 122.88, and 245.76 MSPS. Added feasibility analysis at those rates.
8. **Real-valued vs. complex FFT path disambiguated.** Wefers' R2C variant is for audio; for IQ, C2C is mandatory.
9. **Block-size / partition-count table extended** to realistic USRP operating points.
10. **Filter exchange via FD crossfading** (Wefers §5.7) added as the production channel-update mechanism.

## References

- Wefers, F. (2015). *Partitioned convolution algorithms for real-time auralization.* PhD thesis, RWTH Aachen. Ch. 5: Uniformly partitioned convolution algorithms.
- Armelloni, E., Giottoli, C., & Farina, A. (2003). *Implementation of real-time partitioned convolution on a DSP board.* IEEE WASPAA.
- Stockham, T. G. Jr. (1966). *High-speed convolution and correlation.* AFIPS Spring Joint Computer Conf.
- Torger, A. & Farina, A. (2001). *Real-Time Partitioned Convolution for Ambiophonics Surround Sound.* IEEE WASPAA.

---

## 1. Parameters

| Symbol | Name | Description | Typical value |
|---|---|---|---|
| `B` | Block length (samples) | Samples per RX/TX frame. Equals UHD `recv()` size. | 512–4096 |
| `N` | Filter length (taps) | Number of taps in `h(n)`. | ≤ 4096 |
| `K` | FFT size | Transform length. `K = 2B`. | 1024–8192 |
| `L` | Sub-filter length | Length of each filter partition. **Must equal `B`.** | = B |
| `P` | Number of partitions | `P = ceil(N/B)`. | ≤ 8 typical |
| `fs` | Sample rate (Hz) | USRP baseband sample rate. | 1 MHz – 245.76 MHz |
| `T_block` | Block period | `B / fs`. The real-time processing budget. | block-rate dependent |

**Hard constraints:**

- `L = B` (else FDL delays misalign with block boundaries — see §4.4).
- `K ≥ 2B` with `K = 2B` being optimal for no wasted padding.
- `T_proc + T_io < T_block` where `T_proc` is per-block compute time and `T_io` is UHD send/recv overhead.

**Latency (steady-state):** `2B / fs` with the ping-pong architecture described in §7. The theoretical minimum of `B / fs` is only reachable with zero-latency in-line processing, which is incompatible with UHD's bulk-transfer streaming model.

---

## 2. Data Structures

### 2.1 Sub-filter spectra — `H_parts[p]`, p = 0..P-1

Each `H_parts[p]` is a length-`K` complex vector holding the DFT of the `p`-th sub-filter, **pre-scaled by `1/K`** so no normalization is needed after the per-block IFFT. Read-only at steady state; rebuilt when the channel changes.

Size: `P · K · sizeof(complex64) = 8 · 1024 · 8 = 64 KB` at defaults.

### 2.2 Input buffer — `input_buf[0..K-1]`

Time-domain sliding window. Left half holds previous block; right half holds current block. This is the overlap-save window.

### 2.3 Frequency-Domain Delay Line — `FDL[p]`, p = 0..P-1

Shift register of `P` input spectra, each length `K`. `FDL[newest]` holds the most recent input spectrum; `FDL[newest-p mod P]` holds the spectrum from `p` blocks ago. Must be **explicitly zero-initialized** before the first block.

Implemented as a circular buffer with a write pointer (index), not by physically shifting spectra.

### 2.4 Scratch buffers

- `fft_scratch_in[0..K-1]`, `fft_scratch_out[0..K-1]` — complex, reused by the FFT plans.
- `Y_accum[0..K-1]` — complex accumulator for the MAC loop.

---

## 3. Algorithm

### 3.1 Phase 1 — Filter preprocessing (init / channel update)

```
INPUTS:  h[0..N-1]   — channel impulse response (complex-valued for baseband IQ)
         B, K=2B, P=ceil(N/B)

PROCEDURE:
  1. Zero-pad h to length P*B:
       h_padded = [h[0..N-1], zeros(P*B - N)]

  2. For each partition p = 0..P-1:
       a. h_sub      = h_padded[p*B .. (p+1)*B - 1]       (length B)
       b. h_sub_pad  = [h_sub, zeros(B)]                   (length K = 2B)
       c. H_raw      = FFT(h_sub_pad, K)                   (length K)
       d. H_parts[p] = H_raw * (1/K)                       (bake in IFFT scale)

OUTPUT: H_parts[0..P-1]   (complex spectra, pre-scaled)
```

Baking `1/K` into `H_parts` removes the post-IFFT normalization from the real-time loop.

### 3.2 Phase 2 — Stream processing (per-block)

```
INPUTS:  x_new[0..B-1]     — new block of B complex samples from USRP RX
         input_buf          — persistent length-K complex buffer
         FDL[0..P-1]        — persistent shift register, K-complex spectra each
         write_ptr          — FDL circular-buffer index, persistent
         H_parts[0..P-1]    — from Phase 1

PROCEDURE:
  Step 1 — Slide input buffer:
       input_buf[0 .. B-1]   <- input_buf[B .. K-1]        (prev right half -> left)
       input_buf[B .. K-1]   <- x_new[0 .. B-1]            (new block -> right)

  Step 2 — Forward FFT:
       X_curr = FFT(input_buf, K)

  Step 3 — Store in FDL (circular):
       write_ptr = (write_ptr + 1) mod P
       FDL[write_ptr] = X_curr

  Step 4 — Frequency-domain MAC:
       Y_accum[0..K-1] = 0
       for p = 0..P-1:
           idx = (write_ptr - p + P) mod P
           for k = 0..K-1:
               Y_accum[k] += H_parts[p][k] * FDL[idx][k]

  Step 5 — Inverse FFT (no post-scaling; H_parts was pre-scaled):
       y_buf = IFFT(Y_accum, K)

  Step 6 — Overlap-save extraction (keep complex):
       y_out[0..B-1] = y_buf[B .. K-1]

OUTPUT: y_out[0..B-1]   (B complex samples, ready for USRP TX)
```

**Key correction vs. v1:** `y_out` is complex, not `real(y_buf[B..K-1])`. For a passband IQ channel, the imaginary part carries real signal energy and dropping it is an error.

### 3.3 Startup transient

The FDL fills over the first `P-1` blocks. During this interval the output corresponds to partial convolution (missing partitions are zero). From block `P-1` onward the output equals `conv(x, h)` exactly (within floating-point error). For `P = 8` and `B = 512` that is 3,584 samples of transient — negligible at any realistic stream duration.

### 3.4 Overflow / state-reset protocol

**Trigger:** RX metadata reports `ERROR_CODE_OVERFLOW`, or `num_rx_samps < B` on a non-final call.

**Action:** the input stream has a discontinuity; the FDL is now stale.

```
Step A: Flush UHD RX buffer (issue stop_continuous, recv until empty, reissue start_continuous).
Step B: Zero the input_buf and all FDL slots.
Step C: Reset write_ptr = 0.
Step D: Resume streaming. The output will have another P-1 block transient before steady state.
```

Without this reset, the FDL would hold spectra of samples that are not actually continuous with the new input, producing garbage for `P` blocks until the stale entries rotate out.

---

## 4. Why this works

### 4.1 Filter decomposition

```
h(n) = sum_{p=0}^{P-1} h_p(n - p*B)          where h_p is length B
```

### 4.2 Convolution decomposition

By linearity:

```
y(n) = x(n) * h(n) = sum_{p=0}^{P-1} (x * h_p)(n - p*B)
```

Each sub-convolution `x * h_p` is computed via K-point circular convolution (FFT multiply, IFFT). The delay `p*B` is realized by the FDL: `FDL[newest-p]` is the spectrum of the input block from `p` blocks ago.

### 4.3 Why `K = 2B`

For overlap-save to produce `B` alias-free output samples from a K-point circular convolution, we need `K ≥ B + L`. With `L = B`, that gives `K ≥ 2B`, and `K = 2B` wastes no samples. Wefers (§5.2) notes that K=2B technically allows up to `B+1` non-aliasing filter coefficients per DFT period; the standard algorithm uses only `B` and leaves one sample free, in exchange for `L = B` exactly aligning with the FDL.

### 4.4 Why `L` must equal `B`

The FDL stores one spectrum per input block and thus can only realize delays that are integer multiples of `B`. The sub-filter offsets are `0, L, 2L, …, (P-1)L`. For these to align with FDL slots, `L = B` is required.

If `L ≠ B` (e.g. `L = B/2`), the FDL would need sub-block granularity, which means either (a) computing FFTs every `L` samples at sub-block rate, eliminating the efficiency advantage, or (b) using a generalized (GUPOLS) structure with extra delay lines — which is what Wefers develops in §5.5 but is out of scope here.

---

## 5. Computational cost (per block)

For **complex-valued** IQ (the USRP case), all FFTs are C2C of size `K`.

| Operation | Count | Cost (complex ops) |
|---|---|---|
| Forward FFT (size K) | 1 | `(K/2) · log2(K)` butterflies |
| Complex spectral MAC | P | `P · K` complex multiplies + `P · K` complex adds |
| Inverse FFT (size K) | 1 | `(K/2) · log2(K)` butterflies |
| Input slide (memcpy) | 1 | `B` complex copies |
| Overlap-save extract (memcpy) | 1 | `B` complex copies |

Per-sample cost (divide by `B`):

```
T_stream ≈ (1/B) · [K · log2(K) + 8·P·K]   (complex-ops equivalent)
```

For `B=512, K=1024, P=8`: ~148 complex ops/sample. Compared to direct FIR at `2N-1 = 8191` real MACs/sample, UPOLS is ~55× cheaper.

**Note:** if you ever switch to real-valued input (not applicable to USRP IQ), R2C/C2R FFTs halve the FFT cost and the MAC loop runs on `K/2+1 = B+1` complex values instead of `K`. Wefers §5.2.1 derives the exact formula. For our case, stay with C2C.

---

## 6. Design trade-offs at realistic USRP sample rates

### 6.1 Block-size vs. partition-count (N = 4096)

| B | P | K | T_block @ 122.88 MHz | T_block @ 61.44 MHz | T_block @ 10 MHz | Per-sample cost |
|---|---|---|---|---|---|---|
| 128 | 32 | 256 | **1.04 µs** | 2.08 µs | 12.8 µs | ~528 ops |
| 256 | 16 | 512 | **2.08 µs** | 4.17 µs | 25.6 µs | ~274 ops |
| **512** | **8** | **1024** | **4.17 µs** | **8.33 µs** | **51.2 µs** | **~148 ops** |
| 1024 | 4 | 2048 | 8.33 µs | 16.7 µs | 102 µs | ~86 ops |
| 2048 | 2 | 4096 | 16.7 µs | 33.3 µs | 205 µs | ~54 ops |
| 4096 | 1 | 8192 | 33.3 µs | 66.7 µs | 410 µs | ~35 ops (= unpartitioned OLS) |

**Real-time budget at 122.88 MHz is extremely tight.** At `B=512`, you have 4.17 µs per block to run:
- 1× forward FFT of size 1024,
- 8× K=1024 complex MACs,
- 1× inverse FFT of size 1024,
- UHD `recv()` and `send()` overhead,
- any RX/TX metadata handling.

On a modern x86 core with FFTW + AVX2, a 1024-point C2C FFT takes roughly 1–2 µs; the MAC loop at 8×1024 = 8192 complex MACs is ~1 µs with SIMD. That leaves very little margin. **At 122.88 MHz, budget-feasibility is borderline on CPU and should be validated empirically before committing to an architecture.**

### 6.2 Feasibility summary

| Sample rate | Block length | Verdict |
|---|---|---|
| ≤ 10 MHz | B = 512 or 1024 | Comfortable margin on any modern CPU. |
| 25–61.44 MHz | B = 1024 or 2048 | Achievable with careful SIMD and FFTW tuning; use `FFTW_PATIENT` plans. |
| 122.88 MHz | B ≥ 2048 or GPU offload | Borderline on CPU; GPU convolution (cuFFT) or FPGA offload becomes attractive. |
| ≥ 245.76 MHz | GPU or FPGA | CPU alone is impractical. Consider UHD RFNoC blocks or RFSoC DSP. |

### 6.3 Memory

| Buffer | Size at defaults | Formula |
|---|---|---|
| `H_parts` | 64 KB | `P · K · 8 B` |
| `FDL` | 64 KB | `P · K · 8 B` |
| `input_buf`, `Y_accum`, `y_buf` | 24 KB | `3 · K · 8 B` |
| **Total** | **~152 KB** | fits in L2 cache on most cores |

---

## 7. Implementation notes for USRP (C++ / UHD)

### 7.1 Architecture — three-thread ping-pong

```
  +--------------+    rb_in    +---------------+    rb_out   +--------------+
  | RX thread    |  ---------> | Worker thread |  ---------> | TX thread    |
  | recv() → buf |   (lock-    | UPOLS process |   (lock-    | send() ← buf |
  +--------------+    free      +---------------+    free     +--------------+
                      ring)                          ring)
```

Armelloni (2003, §3.2) uses double-buffering (Alfa/Beta); a three-thread ring-buffer design is the modern equivalent and generalizes to N-deep queues if extra jitter tolerance is needed. The worker is stateful (holds `input_buf`, `FDL`, `write_ptr`); RX and TX are stateless data pumps.

**Latency breakdown** (all in samples):
- RX fill of one block: `B`
- Queue wait: `~0` (worker runs ahead)
- UPOLS processing: `< B` required
- TX queue and pre-buffer: `B` (for reliable send timing)
- **Total (steady-state): `2B`**

### 7.2 FFT library

FFTW3 with `fftwf_plan_dft_1d(K, in, out, FFTW_FORWARD, FFTW_MEASURE)` (use `FFTW_PATIENT` or `FFTW_EXHAUSTIVE` in production — plan once, save to wisdom file, reuse). Allocate input and output arrays with `fftwf_alloc_complex` to guarantee SIMD alignment.

### 7.3 FDL as circular buffer (no physical shift)

Physically shifting `P` spectra each block is `P·K` complex writes — expensive. Use a circular index:

```cpp
int write_ptr = 0;
// per block:
write_ptr = (write_ptr + 1) % P;
memcpy(FDL[write_ptr], X_curr, K * sizeof(fftwf_complex));
// MAC loop reads FDL[(write_ptr - p + P) % P]
```

The `% P` hash is one integer op per iteration — no concern. Pre-computing the index table once per block (`int idx[P]`) lets the inner loop use a plain array access and helps the compiler vectorize.

### 7.4 SIMD MAC loop (AVX2 / NEON)

The inner K-point MAC loop is the hot path. With AVX2 (256-bit), you process 4 complex floats per iteration:

```cpp
// Pre-compute FDL indices for this block (8 entries for P=8)
int fdl_idx[P];
for (int p = 0; p < P; p++) fdl_idx[p] = (write_ptr - p + P) % P;

// Zero accumulator
memset(Y_accum, 0, K * sizeof(fftwf_complex));

// Outer loop over partitions, inner loop over frequency bins
for (int p = 0; p < P; p++) {
    const fftwf_complex* H = H_parts[p];
    const fftwf_complex* X = FDL[fdl_idx[p]];
    // AVX2 complex MAC: (a+jb)(c+jd) = (ac-bd) + j(ad+bc)
    // Processes 4 complex per iteration
    for (int k = 0; k < K; k += 4) {
        __m256 h_vec = _mm256_load_ps(&H[k][0]);
        __m256 x_vec = _mm256_load_ps(&X[k][0]);
        __m256 y_vec = _mm256_load_ps(&Y_accum[k][0]);
        // complex multiply via shuffle + fmaddsub
        __m256 h_re = _mm256_moveldup_ps(h_vec);           // (hr, hr, hr, hr)
        __m256 h_im = _mm256_movehdup_ps(h_vec);           // (hi, hi, hi, hi)
        __m256 x_sw = _mm256_shuffle_ps(x_vec, x_vec, 0xB1); // swap re/im of X
        __m256 prod = _mm256_fmaddsub_ps(h_re, x_vec,
                       _mm256_mul_ps(h_im, x_sw));
        y_vec = _mm256_add_ps(y_vec, prod);
        _mm256_store_ps(&Y_accum[k][0], y_vec);
    }
}
```

Benchmarks on recent x86 typically show a 4-6× speedup over scalar code. On ARM/aarch64 (Jetson, etc.), use NEON intrinsics with `vcmlaq_f32` (Armv8.3+ has complex FMA). Note that `volk_32fc_x2_multiply_conjugate_32fc_a` and related GNU Radio VOLK kernels handle this idiomatically and auto-dispatch per platform.

### 7.5 Ping-pong I/O with UHD

```cpp
// ---- TX timestamp scheduling ----
// We MUST schedule TX far enough ahead that samples arrive at the DAC
// on time. Minimum offset = 2*B (ping-pong pipeline) + safety margin.

const size_t SAFETY_SAMPLES = 4 * B;   // 4 blocks of slack (tunable)

uhd::tx_metadata_t tx_md;
tx_md.has_time_spec = true;
tx_md.start_of_burst = true;  // on first send only

// Anchor TX time to first RX timestamp + total pipeline latency
uhd::time_spec_t tx_start = first_rx_time + uhd::time_spec_t(0,
                               2 * B + SAFETY_SAMPLES, fs);

// Each subsequent block: tx_md.time_spec = tx_start + n*B/fs
```

v1's `rx_md.time_spec + B/fs` is wrong — it schedules the first TX sample at the time the *last* sample of the current RX block was captured, which is in the past relative to when processing finishes.

### 7.6 Overflow / underflow handling

```cpp
rx_stream->recv(&rx_buf[0], B, rx_md);

switch (rx_md.error_code) {
    case uhd::rx_metadata_t::ERROR_CODE_NONE:
        break;
    case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
        std::cerr << "RX overflow — resetting FDL" << std::endl;
        reset_upols_state();   // zero input_buf, FDL, write_ptr
        // optionally: resync TX timestamps
        continue;
    case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
        // benign during start-up; log and retry
        continue;
    default:
        std::cerr << "RX error: " << rx_md.strerror() << std::endl;
        running = false;
}

// TX async message queue drain (detect underflows)
uhd::async_metadata_t async_md;
while (tx_stream->recv_async_msg(async_md, 0.0)) {
    if (async_md.event_code == uhd::async_metadata_t::EVENT_CODE_UNDERFLOW ||
        async_md.event_code == uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET) {
        std::cerr << "TX underflow" << std::endl;
        // adjust timing pipeline, possibly widen SAFETY_SAMPLES
    }
}
```

### 7.7 Channel updates at runtime (frequency-domain crossfading)

For a time-varying channel (e.g., Doppler spread, mobility emulation), naively swapping `H_parts` causes audible/visible clicks from the filter impulse-response discontinuity.

**Method (Wefers §5.7, FD crossfading):**

1. Maintain two filter banks: `H_parts_A`, `H_parts_B`.
2. When a new channel `h'` is loaded, compute `H_parts_B` in a background thread.
3. Run two MAC branches against the same FDL, producing `Y_A` and `Y_B`.
4. For `M` blocks (crossfade length, typically `M = 1` to `4`), output:
   ```
   Y_mix[k] = α · Y_A[k] + (1 - α) · Y_B[k]
   ```
   with `α` ramping from 1 → 0 over the crossfade (cosine-squared is smoother than linear).
5. After the crossfade, swap the pointers so `H_parts_A` ← `H_parts_B` and the single-branch hot path resumes.

Cost during crossfade: 2× MAC work plus the linear combination. For `M = 4` blocks at `B = 512`, that's ~16 ms of doubled compute — entirely manageable.

### 7.8 Complete pseudocode (C++ with FFTW, single-threaded skeleton)

```cpp
#include <fftw3.h>
#include <uhd/usrp/multi_usrp.hpp>
#include <complex>
#include <cstring>
#include <vector>

// Hardcoded parameters (per project convention: no argparse / CLI)
static constexpr int B  = 512;
static constexpr int K  = 2 * B;
static constexpr int N  = 4096;
static constexpr int P  = (N + B - 1) / B;   // ceil(N/B) = 8
static constexpr double fs = 10e6;           // USRP sample rate
static constexpr double cf = 2.4e9;          // center frequency
static constexpr size_t SAFETY_SAMPLES = 4 * B;

// Persistent state
fftwf_complex* input_buf;          // length K
fftwf_complex* fft_scratch_out;    // length K
fftwf_complex* Y_accum;            // length K
fftwf_complex* y_buf;              // length K
fftwf_complex H_parts[P][K];
fftwf_complex FDL[P][K];
int write_ptr = 0;

fftwf_plan plan_fwd, plan_inv;

void init_upols(const std::complex<float>* h /* length N */) {
    // Allocate SIMD-aligned buffers
    input_buf       = fftwf_alloc_complex(K);
    fft_scratch_out = fftwf_alloc_complex(K);
    Y_accum         = fftwf_alloc_complex(K);
    y_buf           = fftwf_alloc_complex(K);

    // FFTW plans — use FFTW_MEASURE; save wisdom in production
    plan_fwd = fftwf_plan_dft_1d(K, input_buf,       fft_scratch_out,
                                 FFTW_FORWARD,  FFTW_MEASURE);
    plan_inv = fftwf_plan_dft_1d(K, Y_accum,         y_buf,
                                 FFTW_BACKWARD, FFTW_MEASURE);

    // Zero persistent state
    std::memset(input_buf, 0, K * sizeof(fftwf_complex));
    std::memset(FDL,       0, P * K * sizeof(fftwf_complex));
    write_ptr = 0;

    // Pre-compute H_parts with baked-in 1/K scaling
    fftwf_complex* tmp_in  = fftwf_alloc_complex(K);
    fftwf_complex* tmp_out = fftwf_alloc_complex(K);
    fftwf_plan init_plan = fftwf_plan_dft_1d(K, tmp_in, tmp_out,
                                             FFTW_FORWARD, FFTW_ESTIMATE);
    const float inv_K = 1.0f / static_cast<float>(K);
    for (int p = 0; p < P; ++p) {
        std::memset(tmp_in, 0, K * sizeof(fftwf_complex));
        for (int b = 0; b < B; ++b) {
            int src = p * B + b;
            if (src < N) {
                tmp_in[b][0] = h[src].real();
                tmp_in[b][1] = h[src].imag();
            }
        }
        fftwf_execute(init_plan);
        for (int k = 0; k < K; ++k) {
            H_parts[p][k][0] = tmp_out[k][0] * inv_K;
            H_parts[p][k][1] = tmp_out[k][1] * inv_K;
        }
    }
    fftwf_destroy_plan(init_plan);
    fftwf_free(tmp_in);
    fftwf_free(tmp_out);
}

void reset_upols_state() {
    std::memset(input_buf, 0, K * sizeof(fftwf_complex));
    std::memset(FDL,       0, P * K * sizeof(fftwf_complex));
    write_ptr = 0;
}

void process_block(const std::complex<float>* x_new /* len B */,
                         std::complex<float>* y_out /* len B */) {
    // Step 1: slide input buffer
    std::memmove(input_buf, input_buf + B, B * sizeof(fftwf_complex));
    for (int i = 0; i < B; ++i) {
        input_buf[B + i][0] = x_new[i].real();
        input_buf[B + i][1] = x_new[i].imag();
    }

    // Step 2: forward FFT → fft_scratch_out
    fftwf_execute(plan_fwd);

    // Step 3: store into FDL circular buffer
    write_ptr = (write_ptr + 1) % P;
    std::memcpy(FDL[write_ptr], fft_scratch_out, K * sizeof(fftwf_complex));

    // Step 4: frequency-domain MAC
    std::memset(Y_accum, 0, K * sizeof(fftwf_complex));
    for (int p = 0; p < P; ++p) {
        const int idx = (write_ptr - p + P) % P;
        for (int k = 0; k < K; ++k) {
            const float hr = H_parts[p][k][0], hi = H_parts[p][k][1];
            const float xr = FDL[idx][k][0],   xi = FDL[idx][k][1];
            Y_accum[k][0] += hr * xr - hi * xi;
            Y_accum[k][1] += hr * xi + hi * xr;
        }
    }

    // Step 5: inverse FFT → y_buf (no post-scaling; baked into H_parts)
    fftwf_execute(plan_inv);

    // Step 6: keep last B complex samples (overlap-save extract)
    for (int i = 0; i < B; ++i) {
        y_out[i] = std::complex<float>(y_buf[B + i][0], y_buf[B + i][1]);
    }
}
```

### 7.9 UHD streaming loop (single-thread version)

```cpp
auto rx_stream = usrp->get_rx_stream(stream_args);
auto tx_stream = usrp->get_tx_stream(stream_args);

std::vector<std::complex<float>> rx_buf(B), tx_buf(B);
uhd::rx_metadata_t rx_md;
uhd::tx_metadata_t tx_md;

// Start streaming continuous
uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
cmd.stream_now = true;
rx_stream->issue_stream_cmd(cmd);

// Grab first RX timestamp to anchor TX schedule
rx_stream->recv(&rx_buf[0], B, rx_md, 3.0);
const uhd::time_spec_t tx_start =
    rx_md.time_spec + uhd::time_spec_t(0, 2 * B + SAFETY_SAMPLES, fs);

size_t block_idx = 0;
tx_md.start_of_burst = true;
tx_md.end_of_burst   = false;
tx_md.has_time_spec  = true;

while (running) {
    if (block_idx > 0) {
        rx_stream->recv(&rx_buf[0], B, rx_md, 0.1);
        if (rx_md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            reset_upols_state();
            continue;
        }
        if (rx_md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) continue;
    }

    process_block(rx_buf.data(), tx_buf.data());

    tx_md.time_spec = tx_start + uhd::time_spec_t(0, block_idx * B, fs);
    tx_stream->send(&tx_buf[0], B, tx_md);
    tx_md.start_of_burst = false;

    // Drain async messages (underflow detection)
    uhd::async_metadata_t amd;
    while (tx_stream->recv_async_msg(amd, 0.0)) {
        if (amd.event_code == uhd::async_metadata_t::EVENT_CODE_UNDERFLOW) {
            std::cerr << "TX underflow at block " << block_idx << std::endl;
        }
    }
    block_idx++;
}

// Cleanly terminate burst
tx_md.end_of_burst = true;
tx_stream->send("", 0, tx_md);
rx_stream->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
```

For production use, split the RX/process/TX across three threads with lock-free SPSC queues (see §7.1). The single-thread version above is sufficient for bench testing and validation.

---

## 8. Verification checklist

Before deploying on the USRP, verify the MATLAB / Python prototype against ground truth:

1. **Steady-state accuracy.** After `P-1` startup blocks, UPOLS output must match `conv(x, h)` to float32 precision (max error < 1e-4 single, < 1e-10 double). Test with random complex input and a random complex channel.
2. **Impulse test.** `x = [1+0j, 0, 0, …]` → output equals `h` delayed by `B` samples (the OLS algorithmic delay). Plot real and imaginary parts separately.
3. **Complex-valued channel test.** Use `h[n] = (a[n] + j·b[n])·exp(-j·2πfd·n/fs)` to verify the imaginary-component path is correct. v1's `real()` bug would fail this test.
4. **Block-boundary continuity.** Plot the output across several block transitions. No discontinuities within floating-point precision.
5. **Timing.** Measure per-block processing time. It must be less than `T_block = B/fs`. At `fs = 122.88 MHz, B = 512`, that's 4.17 µs — measure carefully.
6. **Overflow recovery.** Inject a synthetic overflow (e.g., skip one block of input). Verify the output resumes correct steady-state within `P` blocks after `reset_upols_state()`.
7. **Channel update.** Change `h` mid-stream via FD crossfade. Verify smooth transition with no impulse-like artifacts.
8. **Long-run stability.** Run for ≥ 1 hour continuous. Check for drift, underflows, overflows, and timing glitches.

---

## 9. MATLAB prototype relationship

| File | What it does | Limitation |
|---|---|---|
| `UniformlyPartitioned_OVS.mlx` | Partitions `h` only; batch-processes full `x` | Input not partitioned; no FDL; not streaming |
| `ovs_part.mlx` | Same + parallel timing | Input not partitioned; no FDL |
| `rescent_modified.mlx` | Attempts dual partition | Buggy: `x` block size 513 > `x` length 512; no FDL |
| `block_processing.mlx` | Binary split of input | Channel not partitioned; only 2 blocks |
| **`streaming_upols.m`** | **Correct streaming UPOLS + FDL (reference impl)** | — |

The defining feature of `streaming_upols.m` is the frequency-domain delay line: a shift register of `P` input spectra that enables block-by-block streaming. This is the missing mechanism in every earlier MATLAB attempt.

---

## 10. Summary of v1 → v2 corrections (recap)

| # | v1 issue | Severity | v2 fix | Section |
|---|---|---|---|---|
| 1 | `real(y_buf)` on IQ output | High | Keep complex throughout | §3.2 Step 6 |
| 2 | Latency claim of `B` | Medium | Corrected to `2B` steady-state | §1, §7.1 |
| 3 | TX offset = `rx_md.time_spec + B/fs` | High | `rx_md.time_spec + (2B + safety)/fs` | §7.5 |
| 4 | No overflow handling | High | Explicit reset protocol | §3.4, §7.6 |
| 5 | Implicit FDL zero-init | Medium | Explicit `memset` + `reset_upols_state()` | §2.3, §7.8 |
| 6 | Post-IFFT `/K` in hot loop | Low | Pre-scale `H_parts` by `1/K` at init | §3.1, §7.8 |
| 7 | Sample rate quoted as 1–50 MHz | Medium | Extended to 245 MHz with feasibility caveats | §6.1–6.2 |
| 8 | Real vs. complex FFT ambiguity | Low | Clarified: C2C mandatory for USRP IQ | §5 |
| 9 | No channel-update mechanism | Medium | FD crossfade (Wefers §5.7) | §7.7 |

The algorithm itself was correct in v1 — the issues were all in the USRP integration and real-time plumbing around it.
