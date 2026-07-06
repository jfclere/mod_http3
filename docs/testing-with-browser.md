# HTTP/3 Testing with Web Browsers

This guide walks you through setting up a local Apache `httpd` server running `mod_http3` and verifying HTTP/3 (QUIC) connections using Google Chrome or Chromium.

---

## High-Level Overview

We provide two main helper scripts to automate the local build and browser configuration:
1. **`scripts/debugger.sh`**: Builds the module, copies it to the test `httpd` installation, generates self-signed SSL/QUIC certificates, sets up a sample document root, and writes a test configuration file.
2. **`scripts/browser.sh`**: Launches a Chrome/Chromium instance configured to trust the local self-signed certificate and forces the browser to use HTTP/3 for localhost connections.

---

## Step 1: Set Up the Server Environment

First, run the debugger script to compile the module and configure the server files:

```sh
./scripts/debugger.sh
```

### What this script does under the hood:
* Invokes `cmake` and `ninja` to build `mod_http3`.
* Copies `mod_http3.so` into the test HTTPD modules folder.
* Generates a self-signed certificate and key under `container/certs/` (if they do not exist) and copies them to the server configuration directory.
* Populates the test document root (`htdocs`) with a sample page.
* Creates the server configuration file (`conf/httpd.conf`) listening on port **8443**.

---

## Step 2: Start the Apache Server

Start the Apache server in single-process debug mode (`-X`) so you can view all logs directly in your terminal:

```sh
./dependencies/httpd-dist/bin/httpd -D FOREGROUND -f conf/httpd.conf
```

---

## Step 3: Launch Chrome/Chromium with QUIC Enabled

Browsers normally disable HTTP/3 over `localhost` or block untrusted self-signed certificates. Run the helper script to open a clean browser instance with the correct development flags:

```sh
./scripts/browser.sh
```

---

## Step 4: Verify HTTP/3 in the Browser

1. In the newly launched browser window, navigate to `https://localhost:8443/`.
2. Open the browser's Developer Tools by pressing **F12** (or `Ctrl + Shift + I`).
3. Click on the **Network** tab.
4. Right-click on any of the column headers in the table (such as *Name*, *Status*, etc.) and select **Protocol** to show the Protocol column.
5. Reload the page (**F5** or `Ctrl + R`).
6. In the **Protocol** column for the request, you should see **`h3`** (confirming that HTTP/3 was successfully used).
