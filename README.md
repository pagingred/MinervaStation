# MinervaStation

A Qt desktop application that acts as a worker node for the Minerva Archive project.

## Features

- Concurrent download and upload with configurable parallelism
- aria2c integration for accelerated downloads (optional)
- Discord OAuth login
- Real-time dashboard with rank, verified bytes, speed, uptime, and disk usage
- Leaderboard with search, sorting, avatars, and heat-map coloring
- Chunked uploads with exponential-backoff retry logic
- File manifest for crash recovery
- System profiler with network speed test for auto-tuning settings
- Server health check panel
- Settings stored in a local INI file next to the executable

## Requirements

- Qt 6 with Widgets and Network modules (built against 6.11.0 / MSVC 2022 x64)
- C++20
- Windows 10+/macOS/Linux
- Optional: [aria2c](https://aria2.github.io/) on PATH for multi-connection downloads

## Building

Run `qmake` and `nmake` with the MSVC 2022 x64 toolchain. 

## Setup

1. Launch the app and click **Discord Login** to authenticate.
3. Optionally set **Temp Dir** and **Downloads Dir** in the Settings tab.
4. Click **Recommended Settings** to auto-detect optimal concurrency and connection values via a system profile and speed test.
5. Click **Start**.

All configuration is persisted to `MinervaStation.ini`.

## Settings

| Option | Default | Description |
|--------|---------|-------------|
| DL Concurrency | 5 | Parallel downloads |
| UL Concurrency | 5 | Parallel uploads |
| Batch Size | 10 | Jobs fetched per request |
| aria2c Connections | 8 | Connections per aria2c download (1-16) |
| Keep Files | off | Retain files after upload |
| Disk Reserve | 500 MB | Minimum free space to maintain |
| Upload Chunk Size | 8 MB | Size of each upload chunk |
| UI Update Interval | 50 ms | Dashboard refresh rate |

## Views

### Dashboard
<img width="1920" height="1032" alt="image" src="https://github.com/user-attachments/assets/2c1ed1f9-f2ad-4155-9bdb-97215ef022b2" />

### Settings
<img width="1920" height="1032" alt="image" src="https://github.com/user-attachments/assets/909375ca-735c-4a3b-85b9-26729a3dffa6" />

### Log
<img width="1920" height="1032" alt="image" src="https://github.com/user-attachments/assets/e970119a-47f7-497a-9ab0-7fa41d69d5bd" />

### Leaderboard
<img width="1920" height="1032" alt="image" src="https://github.com/user-attachments/assets/8a12a207-4830-47aa-84c2-bdc6be7a2a86" />
