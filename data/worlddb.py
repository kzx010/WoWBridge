"""Read-only queries against the reference realm's world DB (the exact data the CMaNGOS server runs)."""
import os, subprocess
from pathlib import Path

REALM = Path(__file__).resolve().parents[2] / 'WoW/realm'
# The realm runs under rootless Podman (its Docker-compatible socket): the system Docker's data root is NTFS, where
# image layers cannot be unpacked. Start it with: DOCKER_HOST=unix://$XDG_RUNTIME_DIR/podman/podman.sock docker compose up -d
PODMAN = Path(f"/run/user/{os.getuid()}/podman/podman.sock")
ENV = {**os.environ, 'DOCKER_HOST': f'unix://{PODMAN}'} if PODMAN.exists() else None


def _num(v: str):
    if v == 'NULL':
        return None
    for t in (int, float):
        try:
            return t(v)
        except ValueError:
            pass
    return v


def query(sql: str, db: str = 'world') -> list[dict]:
    r = subprocess.run(['docker', 'compose', 'exec', '-T', 'database', 'sh', '-c',
                        ': "${MYSQL_USER:?database user is unset}" "${MYSQL_PASSWORD:?database password is unset}"; '
                        'MYSQL_PWD="$MYSQL_PASSWORD" exec mariadb --user="$MYSQL_USER" --batch "$@"',
                        'wowbridge-query', db, '-e', sql],
                       cwd=REALM, env=ENV, capture_output=True, text=True, check=True)
    lines = r.stdout.rstrip('\n').split('\n') if r.stdout.strip() else []
    if not lines:
        return []
    head = lines[0].split('\t')
    return [dict(zip(head, map(_num, l.split('\t')))) for l in lines[1:]]


def ids(values) -> str:
    return ','.join(str(int(v)) for v in sorted(set(values))) or 'NULL'
