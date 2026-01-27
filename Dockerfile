##############################################################################################
# This file is part of OpenAirLink.
#
# OpenAirLink is free software: you can redistribute it and/or modify it under the terms of 
# the GNU General Public License as published by the Free Software Foundation, either 
# version 3 of the License, or (at your option) any later version.
#
# OpenAirLink is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
# without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
# See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with OpenAirLink.
# If not, see <https://www.gnu.org/licenses/>.
##############################################################################################

FROM ubuntu:22.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# UHD version to build
ARG UHD_VERSION=4.7.0.0

# Install all dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    # Boost libraries
    libboost-all-dev \
    # ZeroMQ
    libzmq3-dev \
    # UHD build dependencies
    libusb-1.0-0-dev \
    python3-dev \
    python3-pip \
    python3-mako \
    python3-numpy \
    python3-requests \
    python3-ruamel.yaml \
    python3-setuptools \
    doxygen \
    dpdk-dev \
    libdpdk-dev \
    # Additional dependencies
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Build UHD 4.7.0 from source
WORKDIR /build/uhd
RUN git clone --depth 1 --branch v${UHD_VERSION} https://github.com/EttusResearch/uhd.git . \
    && mkdir -p host/build \
    && cd host/build \
    && cmake .. \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DENABLE_PYTHON_API=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DENABLE_TESTS=OFF \
    -DENABLE_MAN_PAGES=OFF \
    -DENABLE_MANUAL=OFF \
    && make -j$(nproc) \
    && make install \
    && ldconfig

# Copy and build OpenAirLink
WORKDIR /build
COPY rfnoc-openairlink /build/rfnoc-openairlink

WORKDIR /build/rfnoc-openairlink/build
RUN cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    && make -j$(nproc) \
    && make install \
    && ldconfig

# Copy channel control files and FPGA bitfiles
COPY rfnoc-openairlink/channel_control /opt/openairlink/channel_control
COPY fpga-openairlink /opt/openairlink/fpga

# Set environment variables
ENV UHD_IMAGES_DIR=/usr/local/share/uhd/images
ENV OAL_CHANNEL_DIR=/opt/openairlink/channel_control
ENV OAL_FPGA_DIR=/opt/openairlink/fpga


# Default command - show help
CMD ["oal_4chan", "--help"]
