# Playout Engine - Experiment Runlog

| Exp # | Profile | delay_ms | Miss % | Overhead | Changes Made | Reason / Result |
|---|---|---|---|---|---|---|
| 1 | Profile A | 40 ms | 12.4% | 1.00x | Baseline naive implementation | **INVALID**: Every packet dropped by relay resulted in audio glitching/miss. |
| 2 | Profile B | 60 ms | 53.13% | 1.54x | Added 1-D XOR Forward Error Correction (FEC) with 20ms fixed offset | **INVALID**: Packet loss recovered via FEC, but playout started too early before parity packets arrived. |
| 3 | Profile B | 60 ms | 100.0% | 1.54x | Increased playout offset to 35 ms | **INVALID**: Playout delay pushed frames past the strict 60ms harness deadline threshold. |
| 4 | Profile B | 50 ms | 0.40% | 1.54x | Dynamic MONOTONIC anchor timing with 24ms playout buffer window | **VALID**: FEC parity packets arrived in time to reconstruct lost payloads while staying under the latency cap. |
EOFcat << 'EOF' > RUNLOG.md
# Playout Engine - Experiment Runlog

| Exp # | Profile | delay_ms | Miss % | Overhead | Changes Made | Reason / Result |
|---|---|---|---|---|---|---|
| 1 | Profile A | 40 ms | 12.4% | 1.00x | Baseline naive implementation | **INVALID**: Every packet dropped by relay resulted in audio glitching/miss. |
| 2 | Profile B | 60 ms | 53.13% | 1.54x | Added 1-D XOR Forward Error Correction (FEC) with 20ms fixed offset | **INVALID**: Packet loss recovered via FEC, but playout started too early before parity packets arrived. |
| 3 | Profile B | 60 ms | 100.0% | 1.54x | Increased playout offset to 35 ms | **INVALID**: Playout delay pushed frames past the strict 60ms harness deadline threshold. |
| 4 | Profile B | 50 ms | 0.40% | 1.54x | Dynamic MONOTONIC anchor timing with 24ms playout buffer window | **VALID**: FEC parity packets arrived in time to reconstruct lost payloads while staying under the latency cap. |
