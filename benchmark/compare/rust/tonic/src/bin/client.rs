use std::{
    env,
    error::Error,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
    time::{Duration, Instant},
};

use tokio::sync::mpsc;
use tokio::task::JoinSet;
use tokio_stream::{wrappers::ReceiverStream, StreamExt};
use tonic::transport::Endpoint;

pub mod bench {
    tonic::include_proto!("bench");
}

use bench::echo_client::EchoClient;
use bench::EchoRequest;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Mode {
    Unary,
    ClientStream,
    ServerStream,
    Bidi,
    StreamBench,
}

impl Mode {
    fn parse(value: &str) -> Option<Self> {
        match value {
            "unary" => Some(Self::Unary),
            "client_stream" => Some(Self::ClientStream),
            "server_stream" => Some(Self::ServerStream),
            "bidi" => Some(Self::Bidi),
            "stream_bench" => Some(Self::StreamBench),
            _ => None,
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::Unary => "unary",
            Self::ClientStream => "client_stream",
            Self::ServerStream => "server_stream",
            Self::Bidi => "bidi",
            Self::StreamBench => "stream_bench",
        }
    }
}

#[derive(Clone, Debug)]
struct Config {
    host: String,
    port: u16,
    connections: usize,
    payload_size: usize,
    duration_secs: u64,
    pipeline_depth: usize,
    io_count: usize,
    mode: Mode,
    frames_per_stream: usize,
    frame_window: usize,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            host: "127.0.0.1".to_string(),
            port: 9000,
            connections: 200,
            payload_size: 47,
            duration_secs: 5,
            pipeline_depth: 4,
            io_count: 0,
            mode: Mode::Unary,
            frames_per_stream: 16,
            frame_window: 1,
        }
    }
}

impl Config {
    fn from_args() -> Result<Self, String> {
        Self::from_iter(env::args())
    }

    fn from_iter<I>(args: I) -> Result<Self, String>
    where
        I: IntoIterator<Item = String>,
    {
        let mut config = Self::default();
        let mut args = args.into_iter();
        let _ = args.next();

        while let Some(arg) = args.next() {
            match arg.as_str() {
                "--host" => config.host = next_value(&mut args, "--host")?,
                "--port" => config.port = parse_num(&mut args, "--port")?,
                "--connections" => config.connections = parse_num(&mut args, "--connections")?,
                "--payload-size" => config.payload_size = parse_num(&mut args, "--payload-size")?,
                "--duration" => config.duration_secs = parse_num(&mut args, "--duration")?,
                "--pipeline-depth" => {
                    config.pipeline_depth = parse_num(&mut args, "--pipeline-depth")?
                }
                "--io-count" => config.io_count = parse_num(&mut args, "--io-count")?,
                "--mode" => {
                    let mode = next_value(&mut args, "--mode")?;
                    config.mode =
                        Mode::parse(&mode).ok_or_else(|| format!("unsupported mode '{mode}'"))?;
                }
                "--frames-per-stream" | "--frames" => {
                    config.frames_per_stream = parse_num(&mut args, "--frames-per-stream")?
                }
                "--frame-window" | "--window" => {
                    config.frame_window = parse_num(&mut args, "--frame-window")?
                }
                "--help" | "-h" => {
                    print_usage();
                    std::process::exit(0);
                }
                other => return Err(format!("unknown option: {other}")),
            }
        }

        if config.connections == 0 {
            return Err("connections must be > 0".to_string());
        }
        if config.duration_secs == 0 {
            return Err("duration must be > 0".to_string());
        }
        if config.pipeline_depth == 0 {
            return Err("pipeline-depth must be > 0".to_string());
        }
        if config.frames_per_stream == 0 {
            return Err("frames-per-stream must be > 0".to_string());
        }
        if config.frame_window == 0 {
            return Err("frame-window must be > 0".to_string());
        }

        Ok(config)
    }
}

fn next_value<I>(args: &mut I, name: &str) -> Result<String, String>
where
    I: Iterator<Item = String>,
{
    args.next()
        .ok_or_else(|| format!("missing value for {name}"))
}

fn parse_num<T, I>(args: &mut I, name: &str) -> Result<T, String>
where
    T: std::str::FromStr,
    I: Iterator<Item = String>,
{
    let value = next_value(args, name)?;
    value
        .parse()
        .map_err(|_| format!("invalid value for {name}: {value}"))
}

fn print_usage() {
    println!(
        "Usage: tonic-bench-client [options]\n\
         \x20 --host <host>              Server host (default: 127.0.0.1)\n\
         \x20 --port <port>              Server port (default: 9000)\n\
         \x20 --connections <count>      Number of concurrent channels (default: 200)\n\
         \x20 --payload-size <bytes>     Payload size in bytes (default: 47)\n\
         \x20 --duration <seconds>       Test duration in seconds (default: 5)\n\
         \x20 --pipeline-depth <count>   In-flight RPCs per channel for request/response modes (default: 4)\n\
         \x20 --io-count <count>         Accepted for parity; tonic baseline does not map this knob (default: 0)\n\
         \x20 --mode <mode>              unary|client_stream|server_stream|bidi|stream_bench\n\
         \x20 --frames-per-stream <n>    Frames per stream for stream_bench (default: 16)\n\
         \x20 --frame-window <n>         Frame window for stream_bench (default: 1)\n\
         \x20 --help                     Show this message"
    );
}

#[derive(Default)]
struct WorkerStats {
    requests: u64,
    frames: u64,
    bytes: u64,
    latencies_us: Vec<u64>,
}

struct CallReport {
    bytes: u64,
    frames: u64,
}

async fn progress_task(
    deadline: Instant,
    mode: Mode,
    total_requests: Arc<AtomicU64>,
    total_frames: Arc<AtomicU64>,
    total_bytes: Arc<AtomicU64>,
) {
    let mut ticker = tokio::time::interval(Duration::from_secs(1));
    let mut last_requests = 0_u64;
    let mut last_frames = 0_u64;
    let mut last_bytes = 0_u64;
    let mut second = 0_u64;

    loop {
        ticker.tick().await;
        if Instant::now() >= deadline {
            break;
        }

        second += 1;
        let current_requests = total_requests.load(Ordering::Relaxed);
        let current_frames = total_frames.load(Ordering::Relaxed);
        let current_bytes = total_bytes.load(Ordering::Relaxed);
        let delta_requests = current_requests.saturating_sub(last_requests);
        let delta_frames = current_frames.saturating_sub(last_frames);
        let throughput_mb = (current_bytes.saturating_sub(last_bytes) as f64) / (1024.0 * 1024.0);

        if mode == Mode::StreamBench {
            println!(
                "[{:>3}s] Streams/s: {:>8} | Frames/s: {:>10} | Throughput: {:.2} MB/s",
                second, delta_requests, delta_frames, throughput_mb
            );
        } else {
            println!(
                "[{:>3}s] QPS: {:>8} | Throughput: {:.2} MB/s",
                second, delta_requests, throughput_mb
            );
        }

        last_requests = current_requests;
        last_frames = current_frames;
        last_bytes = current_bytes;
    }
}

fn percentile(sorted: &[u64], numerator: usize, denominator: usize) -> u64 {
    if sorted.is_empty() {
        return 0;
    }
    let rank = ((sorted.len() - 1) * numerator) / denominator;
    sorted[rank]
}

async fn run_single_call(
    client: &mut EchoClient<tonic::transport::Channel>,
    mode: Mode,
    payload: &[u8],
    frames_per_stream: usize,
    frame_window: usize,
) -> Result<CallReport, Box<dyn Error + Send + Sync>> {
    let payload_vec = payload.to_vec();
    match mode {
        Mode::Unary => {
            let response = client
                .unary_echo(tonic::Request::new(EchoRequest {
                    payload: payload_vec.clone(),
                }))
                .await?;
            if response.get_ref().payload != payload_vec {
                return Err("response payload mismatch".into());
            }
            Ok(CallReport {
                bytes: (payload.len() * 2) as u64,
                frames: 0,
            })
        }
        Mode::ClientStream => {
            let outbound = tokio_stream::iter([EchoRequest {
                payload: payload_vec.clone(),
            }]);
            let response = client
                .client_stream_echo(tonic::Request::new(outbound))
                .await?;
            if response.get_ref().payload != payload_vec {
                return Err("response payload mismatch".into());
            }
            Ok(CallReport {
                bytes: (payload.len() * 2) as u64,
                frames: 0,
            })
        }
        Mode::ServerStream => {
            let mut inbound = client
                .server_stream_echo(tonic::Request::new(EchoRequest {
                    payload: payload_vec.clone(),
                }))
                .await?
                .into_inner();
            let message = inbound
                .message()
                .await?
                .ok_or("server stream ended before first frame")?;
            if message.payload != payload_vec {
                return Err("response payload mismatch".into());
            }
            if inbound.message().await?.is_some() {
                return Err("server stream produced unexpected extra frame".into());
            }
            Ok(CallReport {
                bytes: (payload.len() * 2) as u64,
                frames: 0,
            })
        }
        Mode::Bidi => {
            let outbound = tokio_stream::iter([EchoRequest {
                payload: payload_vec.clone(),
            }]);
            let mut inbound = client
                .bidi_echo(tonic::Request::new(outbound))
                .await?
                .into_inner();
            let message = inbound
                .message()
                .await?
                .ok_or("bidi stream ended before first frame")?;
            if message.payload != payload_vec {
                return Err("response payload mismatch".into());
            }
            if inbound.message().await?.is_some() {
                return Err("bidi stream produced unexpected extra frame".into());
            }
            Ok(CallReport {
                bytes: (payload.len() * 2) as u64,
                frames: 0,
            })
        }
        Mode::StreamBench => {
            let max_inflight = frame_window.max(1).min(frames_per_stream);
            let (sender, receiver) = mpsc::channel::<EchoRequest>(max_inflight * 2 + 1);
            let mut inbound = client
                .bidi_echo(tonic::Request::new(ReceiverStream::new(receiver)))
                .await?
                .into_inner();

            let mut sent = 0_usize;
            let mut received = 0_usize;
            while received < frames_per_stream {
                while sent < frames_per_stream && sent.saturating_sub(received) < max_inflight {
                    sender
                        .send(EchoRequest {
                            payload: payload_vec.clone(),
                        })
                        .await
                        .map_err(|_| "bidi sender closed unexpectedly")?;
                    sent += 1;
                }

                let frame = inbound
                    .message()
                    .await?
                    .ok_or("bidi stream ended before all frames were echoed")?;
                if frame.payload != payload_vec {
                    return Err("response payload mismatch".into());
                }
                received += 1;
            }
            drop(sender);
            if inbound.next().await.is_some() {
                return Err("bidi stream produced unexpected extra frame".into());
            }

            Ok(CallReport {
                bytes: (payload.len() * frames_per_stream * 2) as u64,
                frames: frames_per_stream as u64,
            })
        }
    }
}

async fn run_worker(
    config: Arc<Config>,
    deadline: Instant,
    total_requests: Arc<AtomicU64>,
    total_frames: Arc<AtomicU64>,
    total_bytes: Arc<AtomicU64>,
) -> Result<WorkerStats, Box<dyn Error + Send + Sync>> {
    let endpoint = Endpoint::from_shared(format!("http://{}:{}", config.host, config.port))?
        .connect_timeout(Duration::from_secs(3))
        .tcp_nodelay(true);
    let channel = endpoint.connect().await?;
    let base_client = EchoClient::new(channel);
    let payload = vec![b'X'; config.payload_size];
    let mut stats = WorkerStats::default();

    let batch_depth = if config.mode == Mode::StreamBench {
        1
    } else {
        config.pipeline_depth.max(1)
    };

    while Instant::now() < deadline {
        let mut jobs = JoinSet::new();
        let planned = batch_depth.min(64);
        for _ in 0..planned {
            if Instant::now() >= deadline {
                break;
            }
            let mut client = base_client.clone();
            let payload_copy = payload.clone();
            let mode = config.mode;
            let frames_per_stream = config.frames_per_stream;
            let frame_window = config.frame_window;
            jobs.spawn(async move {
                let started = Instant::now();
                let report = run_single_call(
                    &mut client,
                    mode,
                    &payload_copy,
                    frames_per_stream,
                    frame_window,
                )
                .await?;
                Ok::<(CallReport, u64), Box<dyn Error + Send + Sync>>((
                    report,
                    started.elapsed().as_micros() as u64,
                ))
            });
        }

        while let Some(joined) = jobs.join_next().await {
            let (report, latency_us) = joined??;
            stats.requests += 1;
            stats.frames += report.frames;
            stats.bytes += report.bytes;
            stats.latencies_us.push(latency_us);
            total_requests.fetch_add(1, Ordering::Relaxed);
            total_frames.fetch_add(report.frames, Ordering::Relaxed);
            total_bytes.fetch_add(report.bytes, Ordering::Relaxed);
        }
    }

    Ok(stats)
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let config = Config::from_args().map_err(|err| {
        eprintln!("{err}");
        print_usage();
        std::io::Error::new(std::io::ErrorKind::InvalidInput, err)
    })?;

    println!("=== tonic Benchmark Client ===");
    println!("Target: {}:{}", config.host, config.port);
    println!("Connections: {}", config.connections);
    println!("Payload size: {} bytes", config.payload_size);
    println!("Duration: {} seconds", config.duration_secs);
    println!("Mode: {}", config.mode.as_str());
    println!("Pipeline depth: {}", config.pipeline_depth);
    println!(
        "IO count: {} (accepted for parity, tonic baseline uses Tokio runtime defaults)",
        config.io_count
    );
    if config.mode == Mode::StreamBench {
        println!("Frames per stream: {}", config.frames_per_stream);
        println!("Frame window: {}", config.frame_window);
        println!("Pipeline depth note: stream_bench keeps one active stream per channel.");
    }
    println!();

    let config = Arc::new(config);
    let deadline = Instant::now() + Duration::from_secs(config.duration_secs);
    let total_requests = Arc::new(AtomicU64::new(0));
    let total_frames = Arc::new(AtomicU64::new(0));
    let total_bytes = Arc::new(AtomicU64::new(0));
    let progress_handle = tokio::spawn(progress_task(
        deadline,
        config.mode,
        Arc::clone(&total_requests),
        Arc::clone(&total_frames),
        Arc::clone(&total_bytes),
    ));

    let start = Instant::now();
    let mut tasks = JoinSet::new();
    for _ in 0..config.connections {
        tasks.spawn(run_worker(
            Arc::clone(&config),
            deadline,
            Arc::clone(&total_requests),
            Arc::clone(&total_frames),
            Arc::clone(&total_bytes),
        ));
    }

    let mut combined = WorkerStats::default();
    while let Some(joined) = tasks.join_next().await {
        let worker = joined??;
        combined.requests += worker.requests;
        combined.frames += worker.frames;
        combined.bytes += worker.bytes;
        combined.latencies_us.extend(worker.latencies_us);
    }
    let _ = progress_handle.await;

    let elapsed = start.elapsed();
    combined.latencies_us.sort_unstable();

    let avg_latency_us = if combined.latencies_us.is_empty() {
        0.0
    } else {
        combined.latencies_us.iter().sum::<u64>() as f64 / combined.latencies_us.len() as f64
    };
    let qps = if elapsed.as_secs_f64() > 0.0 {
        combined.requests as f64 / elapsed.as_secs_f64()
    } else {
        0.0
    };
    let frames_per_sec = if elapsed.as_secs_f64() > 0.0 {
        combined.frames as f64 / elapsed.as_secs_f64()
    } else {
        0.0
    };
    let throughput_mb = if elapsed.as_secs_f64() > 0.0 {
        (combined.bytes as f64 / (1024.0 * 1024.0)) / elapsed.as_secs_f64()
    } else {
        0.0
    };

    println!();
    println!("=== tonic Benchmark Report ===");
    println!("Total Requests:    {}", combined.requests);
    if config.mode == Mode::StreamBench {
        println!("Total Frames:      {}", combined.frames);
        println!("Avg Streams/s:     {:.2}", qps);
        println!("Avg Frames/s:      {:.2}", frames_per_sec);
    } else {
        println!("Avg QPS:           {:.2}", qps);
    }
    println!("Total Bytes:       {}", combined.bytes);
    println!("Test Duration:     {:.2} s", elapsed.as_secs_f64());
    println!("Avg Throughput:    {:.2} MB/s", throughput_mb);
    println!("Avg Latency:       {:.2} us", avg_latency_us);
    println!(
        "P50 Latency:       {} us",
        percentile(&combined.latencies_us, 50, 100)
    );
    println!(
        "P95 Latency:       {} us",
        percentile(&combined.latencies_us, 95, 100)
    );
    println!(
        "P99 Latency:       {} us",
        percentile(&combined.latencies_us, 99, 100)
    );

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{Config, Mode};

    #[test]
    fn parses_custom_overrides() {
        let config = Config::from_iter([
            "tonic-bench-client".to_string(),
            "--host".to_string(),
            "127.0.0.2".to_string(),
            "--port".to_string(),
            "9100".to_string(),
            "--connections".to_string(),
            "12".to_string(),
            "--payload-size".to_string(),
            "64".to_string(),
            "--duration".to_string(),
            "3".to_string(),
            "--pipeline-depth".to_string(),
            "8".to_string(),
            "--io-count".to_string(),
            "2".to_string(),
            "--mode".to_string(),
            "bidi".to_string(),
            "--frames-per-stream".to_string(),
            "32".to_string(),
            "--frame-window".to_string(),
            "4".to_string(),
        ])
        .expect("config should parse");

        assert_eq!(config.host, "127.0.0.2");
        assert_eq!(config.port, 9100);
        assert_eq!(config.connections, 12);
        assert_eq!(config.payload_size, 64);
        assert_eq!(config.duration_secs, 3);
        assert_eq!(config.pipeline_depth, 8);
        assert_eq!(config.io_count, 2);
        assert_eq!(config.mode, Mode::Bidi);
        assert_eq!(config.frames_per_stream, 32);
        assert_eq!(config.frame_window, 4);
    }

    #[test]
    fn rejects_unknown_mode() {
        let err = Config::from_iter([
            "tonic-bench-client".to_string(),
            "--mode".to_string(),
            "unknown".to_string(),
        ])
        .expect_err("mode should be rejected");

        assert!(err.contains("unsupported mode"));
    }
}
