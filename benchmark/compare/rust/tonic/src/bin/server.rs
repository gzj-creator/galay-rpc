use std::{env, net::SocketAddr};

use tokio::sync::mpsc;
use tokio_stream::wrappers::ReceiverStream;
use tonic::{transport::Server, Request, Response, Status};

pub mod bench {
    tonic::include_proto!("bench");
}

use bench::echo_server::{Echo, EchoServer};
use bench::{EchoRequest, EchoResponse};

#[derive(Default)]
struct EchoService;

#[tonic::async_trait]
impl Echo for EchoService {
    async fn unary_echo(
        &self,
        request: Request<EchoRequest>,
    ) -> Result<Response<EchoResponse>, Status> {
        Ok(Response::new(EchoResponse {
            payload: request.into_inner().payload,
        }))
    }

    async fn client_stream_echo(
        &self,
        request: Request<tonic::Streaming<EchoRequest>>,
    ) -> Result<Response<EchoResponse>, Status> {
        let mut inbound = request.into_inner();
        let mut payload = Vec::new();

        while let Some(message) = inbound.message().await? {
            payload = message.payload;
        }

        Ok(Response::new(EchoResponse { payload }))
    }

    type ServerStreamEchoStream = ReceiverStream<Result<EchoResponse, Status>>;

    async fn server_stream_echo(
        &self,
        request: Request<EchoRequest>,
    ) -> Result<Response<Self::ServerStreamEchoStream>, Status> {
        let (sender, receiver) = mpsc::channel(1);
        let payload = request.into_inner().payload;
        let _ = sender.send(Ok(EchoResponse { payload })).await;
        Ok(Response::new(ReceiverStream::new(receiver)))
    }

    type BidiEchoStream = ReceiverStream<Result<EchoResponse, Status>>;

    async fn bidi_echo(
        &self,
        request: Request<tonic::Streaming<EchoRequest>>,
    ) -> Result<Response<Self::BidiEchoStream>, Status> {
        let mut inbound = request.into_inner();
        let (sender, receiver) = mpsc::channel(64);

        tokio::spawn(async move {
            loop {
                match inbound.message().await {
                    Ok(Some(message)) => {
                        if sender
                            .send(Ok(EchoResponse {
                                payload: message.payload,
                            }))
                            .await
                            .is_err()
                        {
                            break;
                        }
                    }
                    Ok(None) => break,
                    Err(status) => {
                        let _ = sender.send(Err(status)).await;
                        break;
                    }
                }
            }
        });

        Ok(Response::new(ReceiverStream::new(receiver)))
    }
}

#[derive(Clone, Debug)]
struct Config {
    host: String,
    port: u16,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            host: "0.0.0.0".to_string(),
            port: 9000,
        }
    }
}

impl Config {
    fn from_args() -> Result<Self, String> {
        let mut config = Self::default();
        let mut args = env::args().skip(1);

        while let Some(arg) = args.next() {
            match arg.as_str() {
                "--host" => {
                    let value = args.next().ok_or("missing value for --host")?;
                    config.host = value;
                }
                "--port" => {
                    let value = args.next().ok_or("missing value for --port")?;
                    config.port = value
                        .parse()
                        .map_err(|_| format!("invalid port: {value}"))?;
                }
                "--help" | "-h" => {
                    print_usage();
                    std::process::exit(0);
                }
                other => return Err(format!("unknown option: {other}")),
            }
        }

        Ok(config)
    }
}

fn print_usage() {
    println!(
        "Usage: tonic-bench-server [--host <host>] [--port <port>]\n  --host   Bind host (default: 0.0.0.0)\n  --port   Listen port (default: 9000)\n  --help   Show this message"
    );
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let config = Config::from_args().map_err(|err| {
        eprintln!("{err}");
        print_usage();
        std::io::Error::new(std::io::ErrorKind::InvalidInput, err)
    })?;
    let addr: SocketAddr = format!("{}:{}", config.host, config.port).parse()?;

    println!("=== tonic Benchmark Server ===");
    println!("Listening on: {addr}");
    println!("Service: bench.Echo/*");

    Server::builder()
        .add_service(EchoServer::new(EchoService))
        .serve_with_shutdown(addr, async {
            let _ = tokio::signal::ctrl_c().await;
        })
        .await?;

    Ok(())
}
