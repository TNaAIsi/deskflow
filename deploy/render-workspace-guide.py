# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Render the offline installation guide without third-party dependencies."""
import argparse
import base64
import html
import re
from pathlib import Path


def inline(text):
    text = html.escape(text)
    text = re.sub(r"\x60([^\x60]+)\x60", r"<code>\1</code>", text)
    return re.sub(r"\[([^\]]+)\]\((https://[^)]+)\)", r'<a href="\2">\1</a>', text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--guide", default="docs/WorkspaceGuide.zh-CN.md")
    parser.add_argument("--output", required=True)
    parser.add_argument("--images", default="build/validation/ui")
    parser.add_argument("--revision", default="")
    args = parser.parse_args()
    lines = Path(args.guide).read_text(encoding="utf-8").splitlines()
    body, navigation, paragraph = [], [], []
    list_type = None
    section = 0

    def flush():
        if paragraph:
            body.append("<p>" + inline(" ".join(paragraph)) + "</p>")
            paragraph.clear()

    def close_list():
        nonlocal list_type
        if list_type:
            body.append("</" + list_type + ">")
            list_type = None

    for line in lines:
        if line.startswith("# "):
            continue
        if not line.strip():
            flush()
            close_list()
            continue
        heading = re.match(r"^(#{2,4}) (.*)$", line)
        if heading:
            flush()
            close_list()
            section += 1
            level = len(heading.group(1))
            label = heading.group(2)
            body.append(f'<h{level} id="s{section}">{inline(label)}</h{level}>')
            if level == 2:
                navigation.append(f'<a href="#s{section}">{html.escape(label)}</a>')
            continue
        numbered = re.match(r"^\d+\. (.*)$", line)
        bullet = re.match(r"^- (.*)$", line)
        if numbered or bullet:
            flush()
            wanted = "ol" if numbered else "ul"
            if list_type != wanted:
                close_list()
                body.append("<" + wanted + ">")
                list_type = wanted
            body.append("<li>" + inline((numbered or bullet).group(1)) + "</li>")
        else:
            close_list()
            paragraph.append(line.strip())
    flush()
    close_list()
    pictures = []
    for filename, caption in (
        ("workspace-files.png", "文件共享与传输进度"),
        ("workspace-audio.png", "固定音频路由"),
        ("workspace-monitors.png", "独立显示器布局示例"),
    ):
        image = Path(args.images) / filename
        if image.is_file():
            encoded = base64.b64encode(image.read_bytes()).decode("ascii")
            pictures.append(f'<figure><img alt="{caption}" src="data:image/png;base64,{encoded}">'
                            f"<figcaption>{caption}，图中设备与文件为演示数据。</figcaption></figure>")
    gallery = ('<details><summary>查看界面示例</summary>' + "".join(pictures) + "</details>") if pictures else ""
    revision = html.escape(args.revision[:12])
    result = """<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Deskflow 共享工作台使用说明</title><style>
:root{color-scheme:light dark;--bg:#f5f7fb;--paper:#fff;--ink:#17233a;--muted:#53627a;--line:#dce3ef;--accent:#215ed0}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:var(--bg);color:var(--ink);
font:16px/1.85 system-ui,-apple-system,"Segoe UI","Microsoft YaHei",sans-serif}main{max-width:1000px;margin:auto;
padding:40px 48px 64px;background:var(--paper);min-height:100vh}h1{font-size:34px;line-height:1.3;margin:0 0 12px}
h2{font-size:23px;border-top:1px solid var(--line);padding-top:26px;margin-top:36px;scroll-margin-top:20px}
h3{font-size:19px}p{margin:14px 0}li{padding:5px 0}a{color:var(--accent);text-decoration:none}
a:hover{text-decoration:underline}nav{display:flex;flex-wrap:wrap;gap:8px 18px;padding:18px 0}
.meta,figcaption{color:var(--muted);font-size:14px}.badges{display:flex;flex-wrap:wrap;gap:8px;margin:20px 0}
.badges span{border:1px solid var(--line);border-radius:20px;padding:3px 14px;font-size:14px}
code{background:var(--bg);border:1px solid var(--line);border-radius:4px;padding:1px 5px}
details{border:1px solid var(--line);border-radius:10px;padding:14px 18px;margin:20px 0}
summary{cursor:pointer;color:var(--accent)}figure{margin:24px 0}img{max-width:100%;border:1px solid var(--line);border-radius:6px}
footer{margin-top:38px;border-top:1px solid var(--line);padding-top:16px}
@media(prefers-color-scheme:dark){:root{--bg:#111826;--paper:#192336;--ink:#ecf1fa;--muted:#adbbd1;--line:#35445e;--accent:#8fb5ff}}
@media(max-width:640px){main{padding:24px 20px}h1{font-size:28px}nav{display:block}nav a{display:block;margin:6px 0}}
@media print{body,main{background:white;color:black}main{padding:0;max-width:none}nav{display:none}details{display:none}h2{break-after:avoid}}
</style><main><h1>Deskflow 共享工作台</h1>
<p class="meta">1.28.1 Workspace · 安装与功能说明</p>
<div class="badges"><span>Mac 输入源切换</span><span>全屏防误切</span><span>系统声音</span><span>文件复制</span><span>独立显示器</span></div>
"""
    result += "<nav>" + "".join(navigation) + "</nav>" + gallery + "\n".join(body)
    result += f'<footer class="meta">源代码版本：{revision or "见安装包验证记录"}。'
    result += "请同时更新 Windows 与 Mac；实体设备联机验证范围见文末。</footer></main></html>"
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(result, encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
