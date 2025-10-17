# to_jsonl.py
import json, re

SRC = 'repo_with_diffs.log'
OUT = 'commits_with_diffs.jsonl'

with open(SRC, 'rb') as f:
    data = f.read().decode('utf-8', errors='replace')

# 记录以 \x1e 开头；第一条可能没有前置 \x1e，所以先 split 再过滤空白
records = [r for r in data.split('\x1e') if r.strip()]

def parse_record(rec: str):
    # 我们在 --pretty 里最后还多放了一个字段分隔符 %x1f，便于用一次 split 拿到“头部6字段 + 剩余全部作为patch”
    # 格式: sha \x1f date \x1f author \x1f email \x1f subject \x1f body \x1f <full patch...>
    parts = rec.split('\x1f', 6)
    sha, date, author, email, subject, body = (parts + ['']*6)[:6]
    patch = parts[6] if len(parts) > 6 else ''
    # 规范化：去掉可能的开头换行
    patch = patch.lstrip('\n')

    # 可选：从 patch 第一行抽取 “diff --git” 数量做个统计
    file_diffs = len([1 for _ in re.finditer(r'(?m)^diff --git ', patch)])

    return {
        'sha': sha,
        'date': date,
        'author': author,
        'email': email,
        'subject': subject,
        'body': body.strip(),
        'file_diffs': file_diffs,
        'patch': patch
    }

with open(OUT, 'w', encoding='utf-8') as w:
    for rec in records:
        obj = parse_record(rec)
        w.write(json.dumps(obj, ensure_ascii=False))
        w.write('\n')

print(f'Wrote {len(records)} commits to {OUT}')