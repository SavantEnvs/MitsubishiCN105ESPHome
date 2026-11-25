# syntax=docker/dockerfile:1.7
# Commit image for MitsubishiCN105ESPHome (ESPHome CN105 external component).
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
ARG SOURCE_DATE_EPOCH
ARG SANITIZER_FLAGS
ARG COVERAGE_FLAGS=
ARG MAYHEM_JOBS=

# googletest (source, for the upstream unit tests' FetchContent, pointed at /usr/src/googletest).
USER root
RUN [ -n "$SOURCE_DATE_EPOCH" ] && repro-sources-list.sh || true ; \
    apt-get update && apt-get install -yq --no-install-recommends googletest \
 && rm -rf /var/lib/apt/lists/*

# ESPHome (pinned) generates the host-platform core sources the component builds against. Installed
# once, offline-usable, at a fixed $HOME-independent path; build.sh runs it via PYTHONPATH.
ARG ESPHOME_VERSION=2026.11.1
RUN pip3 install --no-cache-dir --break-system-packages --target /opt/toolchains/esphome \
        "esphome==${ESPHOME_VERSION}" \
 && find /opt/toolchains/esphome -name '__pycache__' -prune -exec rm -rf {} + \
 && PYTHONPATH=/opt/toolchains/esphome python3 -m esphome version \
 && chmod -R a+rX /opt/toolchains/esphome
USER mayhem

WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
