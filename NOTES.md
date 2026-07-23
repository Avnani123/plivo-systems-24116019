# Design & Architecture Notes

Our system implements a low-latency real-time audio transport engine built around 1-D XOR Forward Error Correction (FEC) and precise MONOTONIC timing synchronization.
The sender generates alternating FEC parity frames for every pair of 20ms audio payloads, achieving a 1.54x bandwidth overhead well below the 2.0x limit.
On the receiver side, a dedicated playout thread buffers incoming packets in a circular jitter buffer and dynamically calculates the playout timeline relative to estimated frame send time.
Playout pacing is governed strictly by high-resolution POSIX timers using `clock_nanosleep(CLOCK_MONOTONIC)` to eliminate inter-frame drift and scheduling jitter.
Missing audio payloads are reconstructed in-place using XOR decoding upon the arrival of parity frames.
We recommend grading our system at `delay_ms = 50`.
This configuration provides a sufficient buffer window for FEC parity arrival under network jitter while remaining strictly within the deadline cap.
The primary limitation that breaks this architecture is consecutive burst losses exceeding two contiguous frames, which exceeds single-parity XOR recovery capacity.
Additionally, extreme network jitter spikes exceeding the target delay offset will trigger late frame arrivals and subsequent concealment.
