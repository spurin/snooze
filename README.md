<h1 align="center">snooze: Chilling but ready to serve your needs 💤</h1>

<p align="center">
  <img src="snooze_logo.svg" width="30%" alt="Snooze Logo">
</p>

Ever wanted an HTTP server that does *almost nothing*, but does it with style? **snooze** is here to serve you-literally. Based on the same ultra-minimal philosophy as [idle](https://github.com/spurin/idle), **snooze** listens on a single port, sends a static response, and then goes right back to its nap.

## Features

- **Ultra-Lightweight**: Built on a `scratch` base, compiled statically, and stripped for maximum minimalism.
- **Default Port**: `80`, so you don’t have to think too hard.
- **Default Message**: `"Hello from snooze!"`, because sometimes that’s all you need.
- **Flexible Overriding**:
  - **Environment Variables**: `PORT`, `MESSAGE`  
    (Highest priority: if these are set, they override everything else.)
  - **Command-Line Flags**: `--port=YOUR_PORT`, `--message=YOUR_MESSAGE`  
    (Used only if environment variables are **not** set for those fields. You can set either one independently without affecting the other.)
  - **Defaults**: If neither environment variables nor command-line flags are provided, snooze uses `80` and `"Hello from snooze!"`.
- **Graceful Shutdown**: Handles `SIGINT` and `SIGTERM`, letting you put it to bed without fuss.
- **Prebuilt Images**: You can pull directly with Docker, Kubernetes, or any OCI-compatible tool:
```plaintext
docker pull spurin/snooze:latest
```

## Quick Start (Docker)

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

---

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

1. **Create a ConfigMap**:

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
```

2. **Reference it in your Deployment**:

```yaml
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

---

## Cleanup / Removal

- **Docker**: Just stop the container (`Ctrl-C` in foreground or `docker stop <id>` if running in detached mode).
- **Kubernetes**: Remove Deployments, ConfigMaps, or Services by running:
```bash
kubectl delete deployment snooze
kubectl delete deployment snooze-override-cmd
kubectl delete deployment snooze-config-deploy
kubectl delete configmap snooze-config
```

---

## Why snooze?

- **Test & Debug**: Perfect for verifying Kubernetes Ingress, load balancers, or quick dev checks.
- **Simplicity**: No overhead from large frameworks-just a tiny compiled binary.
- **Minimal Attack Surface**: Less code, fewer dependencies-less to go wrong.
- **Education**: Great example of how to create a statically linked, scratch-based HTTP container.

## License

This project is released into the public domain under [Unlicense](http://unlicense.org).  
Feel free to do whatever you want with it-no strings attached.
