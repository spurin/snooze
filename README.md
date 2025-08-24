<!-- filepath: /Users/ganapathichidambaram/git/snooze/README.md -->
<h1 align="center">snooze: Chilling but ready to serve your needs 💤</h1>

<p align="center">
  <img src="snooze_logo.svg" width="30%" alt="Snooze Logo">
</p>

Ever wanted an HTTP server that does *almost nothing*, but does it with style? **snooze** is here to serve you-literally. Based on the same ultra-minimal philosophy as lightweight testing utilities, **snooze** listens on a single port, sends a static response, and then goes right back to its nap.

## Features

- **Ultra-Lightweight**: Small single-binary server with minimal dependencies.
- **Default Port**: `80`, so you don’t have to think too hard.
- **Default Message**: `"Hello from snooze!"`, because sometimes that’s all you need.
- **Flexible Overriding**:
  - **Environment Variables**: `PORT`, `MESSAGE` (Highest priority)
  - **Command-Line Flags**: `--port=YOUR_PORT`, `--message=YOUR_MESSAGE` (Used only if env var not set)
  - **Defaults**: If neither environment variables nor command-line flags are provided, snooze uses `80` and `"Hello from snooze!"`.
- **Graceful Shutdown**: Handles `SIGINT` and `SIGTERM`, letting you put it to bed without fuss.
- **Structured JSON logging**: Easy ingestion by logging systems.

## Quick note
This repository contains a tiny HTTP server intended for testing. See the docs and examples below for how to run it locally, in Docker, or in Kubernetes.

## Docker

Prebuilt image (if available):

```bash
docker pull spurin/snooze:latest
```

### Quick Start (Docker)

**Easiest**: run with default port (80) and message:

```bash
docker run --rm -p 80:80 spurin/snooze:latest
```

Check in your browser at `http://localhost` or:

```bash
curl http://localhost
# Output: "Hello from snooze!"
```

### Override Using Environment Variables

```bash
docker run --rm -p 8080:8080 \
  -e PORT=8080 \
  -e MESSAGE="Custom Snooze Message" \
  spurin/snooze:latest
```

Check:

```bash
curl http://localhost:8080
# Output: "Custom Snooze Message"
```

### Override Using Command-Line Flags

Because environment variables take priority, only use flags if you don’t specify `PORT` or `MESSAGE` via env. You can override **both** or **one**:

```bash
# Override both port and message
docker run --rm -p 9090:9090 \
  spurin/snooze:latest \
  --port=9090 \
  --message="Command line override!"
```

Check:

```bash
curl http://localhost:9090
# Output: "Command line override!"
```

Or just one of them:

```bash
# Only override port (keep default message)
docker run --rm -p 7070:7070 \
  spurin/snooze:latest \
  --port=7070
```

```bash
curl http://localhost:7070
# Output: "Hello from snooze!"
```

## Build (Optional)

If you want to build **snooze** yourself you need the following dependencies:

- gcc or clang
- make
- cmake

To build **snooze**:

```bash
cd build/
cmake ..
make
```

then you will find the _snooze_ binary in the `build/` directory.

## Kubernetes Examples

### 1. Minimal Deployment (No Overrides)

A simple Deployment referencing the public image. By default, **snooze** listens on port 80 and sends `"Hello from snooze!"`.

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: snooze
spec:
  replicas: 1
  selector:
    matchLabels:
      app: snooze
  template:
    metadata:
      labels:
        app: snooze
    spec:
      containers:
      - name: snooze
        image: spurin/snooze:latest
        ports:
        - containerPort: 80
```

### 2. Command-Line Flags Override

If you want to override via command-line flags within Kubernetes YAML, you can specify `args` to pass them directly to the existing ENTRYPOINT. For example:

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: snooze-override-cmd
spec:
  replicas: 1
  selector:
    matchLabels:
      app: snooze-override-cmd
  template:
    metadata:
      labels:
        app: snooze-override-cmd
    spec:
      containers:
      - name: snooze
        image: spurin/snooze:latest
        # Let the container keep its ENTRYPOINT of "/snooze"
        # but override with these flags:
        args:
          - "--port=8080"
          - "--message=Hello from command-line in K8s!"
        ports:
        - containerPort: 8080
```

This tells **snooze** to listen on port **8080** and respond with `"Hello from command-line in K8s!"`.

### 3. Using a ConfigMap for HTML Content

If you prefer to keep your message in a ConfigMap (for example, to serve HTML with `<head>` and `<body>` tags), you can do this:

**Create a ConfigMap and Deployment**:

```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: snooze-config
data:
  message: |
    <html>
    <head><title>Snooze</title></head>
    <body>
      <h1>Hello from snooze ConfigMap!</h1>
      <p>We can store any HTML here.</p>
    </body>
    </html>
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: snooze-config-deploy
spec:
  replicas: 1
  selector:
    matchLabels:
      app: snooze-config
  template:
    metadata:
      labels:
        app: snooze-config
    spec:
      containers:
      - name: snooze
        image: spurin/snooze:latest
        env:
          # This sets the MESSAGE environment variable
          # from the key "message" in the ConfigMap
          - name: MESSAGE
            valueFrom:
              configMapKeyRef:
                name: snooze-config
                key: message
          # Optionally override the port with an env var
          - name: PORT
            value: "8080"
        ports:
        - containerPort: 8080
```

With this setup, **snooze** reads your HTML from the `MESSAGE` environment variable. When you make a request to port **8080**, you’ll receive your entire HTML from the ConfigMap.

### 4. Multi-Path Ingress Example

Below is a demonstration of path-based routing across three color-coded Deployments and Services. Each path (/red, /green, /blue) points to a different instance of snooze, each listening on a distinct port and returning a unique message.

```yaml
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: snooze-red
spec:
  replicas: 1
  selector:
    matchLabels:
      app: snooze-red
  template:
    metadata:
      labels:
        app: snooze-red
    spec:
      containers:
      - name: snooze
        image: spurin/snooze:latest
        args:
          - "--port=8081"
          - "--message=RED!"
        ports:
        - containerPort: 8081
---
apiVersion: v1
kind: Service
metadata:
  name: snooze-red-service
spec:
  selector:
    app: snooze-red
  ports:
    - port: 80
      targetPort: 8081
      protocol: TCP
  type: ClusterIP
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: snooze-green
spec:
  replicas: 1
  selector:
    matchLabels:
      app: snooze-green
  template:
    metadata:
      labels:
        app: snooze-green
    spec:
      containers:
      - name: snooze
        image: spurin/snooze:latest
        args:
          - "--port=8082"
          - "--message=GREEN!"
        ports:
        - containerPort: 8082
---
apiVersion: v1
kind: Service
metadata:
  name: snooze-green-service
spec:
  selector:
    app: snooze-green
  ports:
    - port: 80
      targetPort: 8082
      protocol: TCP
  type: ClusterIP
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: snooze-blue
spec:
  replicas: 1
  selector:
    matchLabels:
      app: snooze-blue
  template:
    metadata:
      labels:
        app: snooze-blue
    spec:
      containers:
      - name: snooze
        image: spurin/snooze:latest
        args:
          - "--port=8083"
          - "--message=BLUE!"
        ports:
        - containerPort: 8083
---
apiVersion: v1
kind: Service
metadata:
  name: snooze-blue-service
spec:
  selector:
    app: snooze-blue
  ports:
    - port: 80
      targetPort: 8083
      protocol: TCP
  type: ClusterIP
---
apiVersion: networking.k8s.io/v1
kind: Ingress
metadata:
  name: snooze-colors-ingress
spec:
  rules:
    - host: snooze.example.com
      http:
        paths:
          - path: /red
            pathType: Prefix
            backend:
              service:
                name: snooze-red-service
                port:
                  number: 80
          - path: /green
            pathType: Prefix
            backend:
              service:
                name: snooze-green-service
                port:
                  number: 80
          - path: /blue
            pathType: Prefix
            backend:
              service:
                name: snooze-blue-service
                port:
                  number: 80
```

Three Deployments (snooze-red, snooze-green, snooze-blue) each run snooze on different ports (8081, 8082, 8083) with distinct messages (RED!, GREEN!, BLUE!).

Three ClusterIP Services route traffic from port 80 to each respective containerPort.

The Ingress resource routes:

- /red to snooze-red-service
- /green to snooze-green-service
- /blue to snooze-blue-service

---

## Cleanup / Removal

- **Docker**: Just stop the container (`Ctrl-C` in foreground or `docker stop <id>` if running in detached mode).
- **Kubernetes**: Remove Deployments, ConfigMaps, or Services by running:
```bash
kubectl delete deployment snooze
kubectl delete deployment snooze-override-cmd
kubectl delete deployment snooze-config-deploy
kubectl delete configmap snooze-config
kubectl delete deployment snooze-red
kubectl delete deployment snooze-green
kubectl delete deployment snooze-blue
kubectl delete svc snooze-red-service snooze-green-service snooze-blue-service
kubectl delete ingress snooze-colors-ingress
```

---

## Why snooze?

- **Test & Debug**: Perfect for verifying Kubernetes Ingress, load balancers, or quick dev checks.
- **Simplicity**: No overhead from large frameworks—just a tiny compiled binary.
- **Minimal Attack Surface**: Less code, fewer dependencies—less to go wrong.
- **Education**: Great example of how to create a statically linked, scratch-based HTTP container.

## License

This project is released into the public domain under [Unlicense](http://unlicense.org).
Feel free to do whatever you want with it—no strings attached.

## Thanks! 🙏

[@edsiper](https://github.com/edsiper) [@ganapathichidambaram](https://github.com/ganapathichidambaram)

## Snooze Delay Behavior

- Endpoint: `GET /snooze/N` where N is a non-negative integer number of seconds.
- The server sleeps for N seconds before sending the response. This simulates a delayed backend.
- `exec_time` for a request measures the total elapsed time for handling that request (including any snooze delay) and is recorded after the response is handled.
- Example: a `/snooze/1` request will yield `exec_time` ≈ `"1.0000"` (formatted with four decimal places).

## Monitoring & Logging (Detailed)

Logs are emitted as JSON objects. Every log entry contains a fixed prefix and may include additional fields specific to the event. The fixed fields are:

- `ts`: ISO8601 timestamp (string)
- `level`: `error` | `info` | `debug` (string)
- `subsystem`: Logical subsystem that produced the log (e.g. `net`, `app`, `http`) (string)
- `exec_time`: A quoted string formatted with four decimals (seconds) that represents the time value relevant to the event

Notes on `exec_time` semantics:

- For HTTP request logs (`subsystem":"http"`), `exec_time` is the total elapsed time to process that request, including any snooze delay. It is measured as seconds with four decimal places and always emitted in double-quotes, e.g. `"1.0000"`.
- For lifecycle and system events (subsystem `app` or `net`), `exec_time` represents a short duration relevant to the event:
  - The `start` log uses elapsed time since process start.
  - When a shutdown signal is received, the server emits `shutdown_requested` with `exec_time` = elapsed since process start, then after completing shutdown it emits a final `shutdown` log whose `exec_time` is the duration taken to complete shutdown (both formatted as quoted strings with four decimals).
- Error logs include the relevant `op` and `error` fields in the JSON body.

Additional fields in request logs:

- `method`: HTTP method (string)
- `path` or `uri`: Request path (string)
- `agent`: `User-Agent` value when present (string)
- Additional headers are included as key/value pairs in the JSON body when present

Example request log (single-line JSON):

{"ts":"2025-08-24T12:34:56+0000","level":"info","subsystem":"http","exec_time":"1.0000","method":"GET","path":"/snooze/1","agent":"curl/7.68.0","Accept":"*/*"}

Tips

- Use a JSON-aware log collector or simple jq to parse and aggregate logs:

```bash
# Example: count requests and avg exec_time
cat logs.json | jq -r -c '. | select(.subsystem=="http") | {exec_time: .exec_time}' | \
  jq -s 'map(tonumber(.exec_time)) | {count: length, avg: (add/length)}'
```

(Adapt tooling to your logging pipeline; exec_time is emitted as a quoted string.)
