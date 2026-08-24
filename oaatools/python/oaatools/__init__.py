"""
oaatools Python SDK & Command Line Engine
"""
import os
import sys
import tarfile
import hashlib
import shutil
import yaml
from pathlib import Path
from dataclasses import dataclass, field
from typing import List, Optional, Dict, Any

@dataclass
class OaaMetadata:
    name: str
    version: str = "1.0.0"
    namespace: str = "app"
    description: str = ""
    architecture: str = "x86_64"
    maintainer: str = ""
    installed_size: int = 0
    dependencies: List[str] = field(default_factory=list)
    files: List[str] = field(default_factory=list)
    checksum: str = ""

    def to_yaml(self) -> str:
        d = {
            "name": self.name,
            "namespace": self.namespace,
            "version": self.version,
            "description": self.description,
            "architecture": self.architecture,
            "maintainer": self.maintainer,
            "installed_size": self.installed_size,
            "dependencies": self.dependencies,
            "files": self.files
        }
        if self.checksum:
            d["checksum"] = self.checksum
        return yaml.safe_dump(d, sort_keys=False)

    @classmethod
    def from_yaml(cls, yaml_content: str) -> "OaaMetadata":
        data = yaml.safe_load(yaml_content) or {}
        return cls(
            name=data.get("name", "unknown"),
            version=str(data.get("version", "1.0.0")),
            namespace=data.get("namespace", "app"),
            description=data.get("description", ""),
            architecture=data.get("architecture", "x86_64"),
            maintainer=data.get("maintainer", ""),
            installed_size=int(data.get("installed_size", 0)),
            dependencies=data.get("dependencies", []),
            files=data.get("files", []),
            checksum=data.get("checksum", "")
        )

class OaaBuilder:
    @staticmethod
    def calculate_sha256(filepath: str) -> str:
        sha = hashlib.sha256()
        with open(filepath, "rb") as f:
            while chunk := f.read(65536):
                sha.update(chunk)
        return sha.hexdigest()

    @staticmethod
    def build(source_dir: str, output_path: Optional[str] = None, compression: str = "zstd") -> str:
        src = Path(source_dir)
        meta_file = src / "meta.yaml"
        if not meta_file.exists():
            meta_file = src / "meta.json"
        if not meta_file.exists():
            raise FileNotFoundError(f"meta.yaml not found in {source_dir}")

        with open(meta_file, "r", encoding="utf-8") as f:
            meta = OaaMetadata.from_yaml(f.read())

        # Pre-build hook
        pre_hook = src / "scripts" / "pre-build"
        if pre_hook.exists() and os.access(pre_hook, os.X_OK):
            os.system(f"cd {src} && ./scripts/pre-build")

        out_file = output_path or f"{meta.name}-{meta.version}.oaa"
        
        # Tar build
        tar_cmd = f"tar --zstd -cf '{out_file}' --exclude=./'{Path(out_file).name}' -C '{src}' . 2>/dev/null"
        if os.system(tar_cmd) != 0:
            tar_cmd = f"tar -czf '{out_file}' -C '{src}' ."
            os.system(tar_cmd)

        # Hash calculation
        sha = OaaBuilder.calculate_sha256(out_file)
        with open(f"{out_file}.sha256", "w", encoding="utf-8") as f:
            f.write(f"{sha}  {Path(out_file).name}\n")

        # Post-build hook
        post_hook = src / "scripts" / "post-build"
        if post_hook.exists() and os.access(post_hook, os.X_OK):
            os.system(f"cd {src} && ./scripts/post-build")

        return out_file

    @staticmethod
    def inspect(archive_path: str) -> Optional[OaaMetadata]:
        if not os.path.exists(archive_path):
            return None
        # Extract meta.yaml stream
        cmd = f"tar --zstd -xf '{archive_path}' -O ./meta.yaml 2>/dev/null || tar -xzf '{archive_path}' -O ./meta.yaml 2>/dev/null || tar -xf '{archive_path}' -O meta.yaml 2>/dev/null"
        stream = os.popen(cmd).read()
        if not stream:
            return None
        return OaaMetadata.from_yaml(stream)

    @staticmethod
    def extract(archive_path: str, dest_dir: str) -> bool:
        os.makedirs(dest_dir, exist_ok=True)
        cmd = f"tar --zstd -xf '{archive_path}' -C '{dest_dir}' 2>/dev/null || tar -xzf '{archive_path}' -C '{dest_dir}' 2>/dev/null || tar -xf '{archive_path}' -C '{dest_dir}'"
        return os.system(cmd) == 0
