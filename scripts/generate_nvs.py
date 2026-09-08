#!/usr/bin/env python3
"""
VorotaBot NVS Partition Generator
Generates a binary NVS partition containing WireGuard, AWS Route 53, and Wi-Fi configurations.
Executed inside Docker container — zero host dependencies.
"""

import os
import sys
import argparse
import subprocess

def parse_wireguard_conf(conf_path_or_str):
    content = ""
    if os.path.isfile(conf_path_or_str):
        with open(conf_path_or_str, "r") as f:
            content = f.read()
    elif "\n" in conf_path_or_str or "[Interface]" in conf_path_or_str:
        content = conf_path_or_str
    else:
        raise FileNotFoundError(f"WireGuard config file not found: {conf_path_or_str}")

    parsed = {
        "wg_priv": "",
        "wg_addr": "10.0.0.2",
        "wg_peer_pub": "",
        "wg_psk": "",
        "wg_endp": "",
        "wg_port": 443,
        "wg_allow": "0.0.0.0/0",
        "wg_ka": 25
    }

    for line in content.splitlines():
        line = line.strip()
        if "=" in line:
            k, v = line.split("=", 1)
            k, v = k.strip().lower(), v.strip()
            if k == "privatekey":
                parsed["wg_priv"] = v
            elif k == "address":
                parsed["wg_addr"] = v.split("/")[0]
            elif k == "publickey":
                parsed["wg_peer_pub"] = v
            elif k == "presharedkey":
                parsed["wg_psk"] = v
            elif k == "endpoint":
                if ":" in v:
                    host, port = v.rsplit(":", 1)
                    parsed["wg_endp"] = host.strip(" []")
                    try:
                        parsed["wg_port"] = int(port.strip())
                    except ValueError:
                        parsed["wg_port"] = 443
                else:
                    parsed["wg_endp"] = v
            elif k == "allowedips":
                parsed["wg_allow"] = v
            elif k == "persistentkeepalive":
                parsed["wg_ka"] = int(v)

    return parsed

def main():
    parser = argparse.ArgumentParser(description="VorotaBot NVS Provisioning Generator")
    parser.add_argument("--wg-config", help="Path to WireGuard client .conf file or config string")
    parser.add_argument("--aws-access-key", help="AWS Access Key ID")
    parser.add_argument("--aws-secret-key", help="AWS Secret Access Key")
    parser.add_argument("--root-domain", default="glebos.click", help="Root domain name (default: glebos.click)")
    parser.add_argument("--hosted-zone-id", help="Route 53 Hosted Zone ID")
    parser.add_argument("--record-name", default="vorota.glebos.click", help="Custom FQDN (default: vorota.glebos.click)")
    parser.add_argument("--wifi-ssid", help="Home Wi-Fi SSID")
    parser.add_argument("--wifi-pass", help="Home Wi-Fi Password")
    parser.add_argument("--out-csv", default="/project/dist/nvs_config.csv", help="Output CSV path")
    parser.add_argument("--out-bin", default="/project/dist/nvs_config.bin", help="Output binary path")
    parser.add_argument("--size", default="0x5000", help="NVS Partition size")

    args = parser.parse_args()
    os.makedirs(os.path.dirname(args.out_bin), exist_ok=True)

    csv_lines = [
        "key,type,encoding,value",
        "config,namespace,,"
    ]

    # WireGuard
    if args.wg_config:
        wg = parse_wireguard_conf(args.wg_config)
        if not wg["wg_priv"] or not wg["wg_peer_pub"] or not wg["wg_endp"]:
            raise ValueError(f"WireGuard config missing required fields! priv={'OK' if wg['wg_priv'] else 'MISSING'}, pub={'OK' if wg['wg_peer_pub'] else 'MISSING'}, endp={'OK' if wg['wg_endp'] else 'MISSING'}")
        csv_lines.append(f"wg_priv,data,string,{wg['wg_priv']}")
        if wg["wg_addr"]: csv_lines.append(f"wg_addr,data,string,{wg['wg_addr']}")
        csv_lines.append(f"wg_peer_pub,data,string,{wg['wg_peer_pub']}")
        if wg.get("wg_psk"): csv_lines.append(f"wg_psk,data,string,{wg['wg_psk']}")
        csv_lines.append(f"wg_endp,data,string,{wg['wg_endp']}")
        csv_lines.append(f"wg_port,data,i32,{wg['wg_port']}")
        csv_lines.append(f"wg_allow,data,string,{wg['wg_allow']}")
        csv_lines.append(f"wg_ka,data,i32,{wg['wg_ka']}")
        csv_lines.append("wg_en,data,i32,1")

    # AWS Route 53
    if args.aws_access_key:
        csv_lines.append(f"aws_key,data,string,{args.aws_access_key}")
    if args.aws_secret_key:
        csv_lines.append(f"aws_sec,data,string,{args.aws_secret_key}")
    if args.root_domain:
        csv_lines.append(f"aws_dom,data,string,{args.root_domain}")
    if args.hosted_zone_id:
        csv_lines.append(f"aws_zone,data,string,{args.hosted_zone_id}")

    record = args.record_name
    if not record and args.root_domain:
        record = f"vorota.{args.root_domain}"
    if record:
        csv_lines.append(f"aws_host,data,string,{record}")

    # Wi-Fi
    if args.wifi_ssid:
        csv_lines.append(f"sta_ssid,data,string,{args.wifi_ssid}")
    if args.wifi_pass:
        csv_lines.append(f"sta_pass,data,string,{args.wifi_pass}")

    csv_content = "\n".join(csv_lines) + "\n"
    with open(args.out_csv, "w") as f:
        f.write(csv_content)

    print(f"[NVS] Generated configuration CSV at {args.out_csv}")

    # Locate nvs_partition_gen.py
    idf_path = os.environ.get("IDF_PATH", "/opt/esp/idf")
    gen_tool = os.path.join(idf_path, "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py")

    cmd = [sys.executable, gen_tool, "generate", args.out_csv, args.out_bin, args.size]
    print(f"[NVS] Invoking: {' '.join(cmd)}")
    subprocess.check_call(cmd)
    print(f"[NVS] SUCCESS: Generated flashable binary at {args.out_bin}")

if __name__ == "__main__":
    main()
