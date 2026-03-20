ARG ARCH=aarch64
ARG VERSION=12.0.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION} as sdk

WORKDIR /opt/app
COPY ./app .

RUN . /opt/axis/acapsdk/environment-setup* && acap-build . && \
    for eap in *.eap; do \
        mkdir -p /tmp/eaptmp && \
        tar xzf "$eap" -C /tmp/eaptmp && \
        cp -r settings html /tmp/eaptmp/ && \
        tar czf "$eap" -C /tmp/eaptmp . && \
        rm -rf /tmp/eaptmp; \
    done
