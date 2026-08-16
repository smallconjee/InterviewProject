#!/bin/bash
# 生成 PBKDF2 哈希并写入 user_account（admin/admin123、doctor/doctor123）
# 算法与 auth 服务端一致：PBKDF2-HMAC-SHA256, 100000 迭代, 16B 盐, 32B 输出，均以十六进制存储
set -e
cd "$(dirname "$0")/.."

MYSQL_CONTAINER="${MYSQL_CONTAINER:-medical-pacs-dev-mysql-1}"

python3 - "$MYSQL_CONTAINER" << 'PYEOF'
import hashlib, os, sys, subprocess

container = sys.argv[1]
users = [("admin", "admin123", "admin"), ("doctor", "doctor123", "radiologist")]

sqls = []
for user, pwd, role in users:
    salt = os.urandom(16)
    h = hashlib.pbkdf2_hmac("sha256", pwd.encode(), salt, 100000, 32)
    sqls.append(
        f"INSERT INTO user_account(username,password_hash,salt,role) VALUES "
        f"('{user}','{h.hex()}','{salt.hex()}','{role}') "
        f"ON DUPLICATE KEY UPDATE password_hash=VALUES(password_hash), "
        f"salt=VALUES(salt), role=VALUES(role);"
    )

p = subprocess.run(["docker", "exec", "-i", container, "mysql", "-uroot", "-proot", "pacs_db"],
                   input="\n".join(sqls), capture_output=True, text=True)
if p.returncode != 0:
    print(p.stderr)
    sys.exit(1)
print("种子用户已写入: admin/admin123(admin), doctor/doctor123(radiologist)")
PYEOF
