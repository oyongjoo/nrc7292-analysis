# Country-specific Regulatory Domain Analysis

## Supported Countries List

| Code | Country/Region | Special Requirements | Bandwidth | Notes |
|------|----------------|----------------------|-----------|-------|
| US   | United States  | None                 | Wide      | Most channels supported |
| JP   | Japan          | None                 | Limited   | Partial 5GHz band only |
| K1   | Korea USN1     | LBT required         | 2MHz      | Non-standard, 921-923MHz |
| K2   | Korea USN5     | MIC detection req.   | 6MHz      | Standard, 925-931MHz |
| TW   | Taiwan         | None                 | Medium    | Various band support |
| EU   | European Union | None                 | Limited   | Partial 5GHz band |
| CN   | China          | None                 | Medium    | 7.xGHz band usage |
| NZ   | New Zealand    | None                 | Wide      | Similar to Australia |
| AU   | Australia      | None                 | Wide      | Same as New Zealand |

## Special Feature Analysis

### LBT (Listen Before Talk) - K1
```c
// Required implementation elements
- Channel sensing mechanism
- Backoff algorithm
- Pre-transmission wait time management
- Interference detection threshold setting
```

### MIC (Mutual Interference Cancellation) - K2
```c
// Required implementation elements
- Interference signal analysis
- Adaptive filtering
- Signal separation algorithm
- Real-time processing optimization
```

## Channel Mapping

Each country maps S1G frequencies to existing WiFi channels to ensure compatibility.

Example: United States
- 9025MHz (S1G) → 2412MHz (WiFi channel 1)
- 9035MHz (S1G) → 2422MHz (WiFi channel 3)
- ...

This maintains compatibility with existing WiFi tools.