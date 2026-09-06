#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Match the source hashes recorded with a hardware record to committed versions.

A record is evidence about the sources it measured, not about today's tree.
Every recorded SHA-256 must equal the file as committed at the record's base
commit, or at a commit AFTER that base which touched the file (a record is
often measured on a tree that is committed a little later). A version older
than the base is never accepted: it cannot have been measured by a run that
started from the base. Only the untracked Cube scaffold may match the working
tree silently; a tracked file whose hash exists only in the working tree is
accepted with a CAVEAT that disappears once the measured sources are committed.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess

CUBE_PREFIX = "stm32_cube_test/"


def file_digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class Provenance:
    def __init__(self, repo):
        self.repo = Path(repo)
        self._blobs = {}
        self._history = {}
        self.checked = set()          # (base, relative, digest) already verified
        self.at_base = 0
        self.later = {}               # (base[:12], commit[:12]) -> set of paths
        self.cube_working = 0
        self.cube_missing = set()
        self.uncommitted = set()

    def _git(self, *args):
        return subprocess.run(["git", *args], cwd=self.repo, capture_output=True)

    def blob_digest(self, revision, relative):
        """SHA-256 of `relative` as committed at `revision`, None if git cannot show it."""
        key = (revision, relative)
        if key not in self._blobs:
            shown = self._git("show", f"{revision}:{relative}")
            self._blobs[key] = hashlib.sha256(shown.stdout).hexdigest() if shown.returncode == 0 else None
        return self._blobs[key]

    def later_commits(self, base, relative):
        """Commits that are NOT ancestors of `base` (reachable from any ref) and
        touched `relative`, oldest first. Anything older than the base is excluded."""
        key = (base, relative)
        if key not in self._history:
            log = self._git("log", "--all", "--reverse", "--format=%H", f"{base}..", "--", relative)
            self._history[key] = log.stdout.decode().split() if log.returncode == 0 else []
        return self._history[key]

    def committed_match(self, base, relative, digest):
        """The commit whose version of `relative` has `digest`: the base itself, else
        the first later commit; None when no version at or after the base matches."""
        if self.blob_digest(base, relative) == digest:
            return base
        for revision in self.later_commits(base, relative):
            if self.blob_digest(revision, relative) == digest:
                return revision
        return None

    def check(self, base, sources):
        """Verify one record's {path: sha256}; raises AssertionError on a hash that
        no committed version at or after the base and no working-tree file has."""
        for relative, digest in sources.items():
            key = (base, relative, digest)
            if key in self.checked:
                continue
            self.checked.add(key)
            matched = self.committed_match(base, relative, digest)
            if matched == base:
                self.at_base += 1
                continue
            if matched is not None:
                self.later.setdefault((base[:12], matched[:12]), set()).add(relative)
                continue
            absolute = self.repo / relative
            if relative.startswith(CUBE_PREFIX):
                if not absolute.exists():
                    self.cube_missing.add(relative)
                    continue
                assert file_digest(absolute) == digest, f"changed Cube source {relative}"
                self.cube_working += 1
                continue
            assert absolute.exists() and file_digest(absolute) == digest, \
                f"{relative}: no committed version at or after base {base[:12]} has the recorded hash"
            self.uncommitted.add(relative)

    def report(self):
        """Print what matched where; the CAVEAT lines are the only acceptable weakenings."""
        lines = [f"sources: {self.at_base} identities match their record's base commit"]
        for (base, revision), paths in sorted(self.later.items()):
            lines.append(f"note: {len(paths)} sources recorded with base {base} match the later commit {revision}")
        if self.cube_working:
            lines.append(f"{self.cube_working} untracked Cube files match the working tree")
        if self.cube_missing:
            lines.append(f"CAVEAT {len(self.cube_missing)} ignored Cube source files unavailable locally")
        if self.uncommitted:
            lines.append(f"CAVEAT {len(self.uncommitted)} measured sources are not committed yet, verified against the working tree: "
                         + ", ".join(sorted(self.uncommitted)))
        for line in lines:
            print(line)
        return lines
