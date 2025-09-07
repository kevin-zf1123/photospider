# save as to_json.py, then: python to_json.py
import json, sys, re

path = 'repo.log'
commits = []
with open(path, 'rb') as f:
    data = f.read().decode('utf-8', errors='replace')

# 每条记录以 \x1e 开头
records = [r for r in data.split('\x1e') if r.strip()]
for rec in records:
    # 先按字段分隔取头部信息
    header, *rest = rec.split('\n', 1)
    parts = header.split('\x1f')
    # 有的记录 body 里也含换行，rest[0] 才是 numstat 等
    body_plus = rest[0] if rest else ''
    # 从 header 取基本字段
    sha, date, author, email, subject, body = (parts + ['']*6)[:6]

    files = []
    for line in body_plus.splitlines():
        # numstat 行形如: "12\t3\tpath/xxx" 或 "12\t3\told => new"
        if '\t' in line:
            cols = line.split('\t')
            if len(cols) >= 3 and cols[0].strip() and cols[1].strip():
                add, delete, path = cols[0], cols[1], '\t'.join(cols[2:])
                files.append({
                    'path': path,
                    'added': 0 if add == '-' else int(add),
                    'deleted': 0 if delete == '-' else int(delete),
                })

    commits.append({
        'sha': sha,
        'date': date,
        'author': author,
        'email': email,
        'subject': subject,
        'body': body.strip(),
        'files': files
    })

with open('changes.json', 'w', encoding='utf-8') as out:
    json.dump(commits, out, ensure_ascii=False, indent=2)

print(f'Wrote {len(commits)} commits to changes.json')