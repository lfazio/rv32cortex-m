FROM debian:trixie-slim

RUN apt-get update && \
    apt-get upgrade -y && \
    apt-get clean -y && \
    apt-get autoremove -y && \
    apt-get autoclean -y

# Install C build environment
RUN apt-get install -y \
    build-essential \
    libicu-dev \
    unzip \
    bash \
    git \
    cmake \
    cppcheck \
    valgrind \
    clang-format \
    clang-tidy \
    llvm \
    clang \
    plantuml \
    doxygen \
    pkg-config

# Install python build environment
RUN apt-get install -y \
    python3 \
    python3-pip \
    python3-setuptools

RUN apt-get install -y \
    cloc

# Install NPM packages
RUN apt-get install -y \
    nodejs \
    npm

RUN npm install -g \
    jscpd

#COPY requirements.txt /tmp/
#RUN pip3 install --no-cache-dir --break-system-packages -r /tmp/requirements.txt

COPY ./cc-rh_v20800/cc-rh-208_2.08.00_amd64.deb /tmp/
RUN yes | apt install -y /tmp/cc-rh-208_2.08.00_amd64.deb

COPY ./license-manager_v20900/license-manager_2.09.00_amd64.deb /tmp/
RUN yes | apt install -y /tmp/license-manager_2.09.00_amd64.deb

# Unit testing framework + coverage
RUN apt-get install -y \
    libcmocka-dev \
    lcov \
    curl

# fff: header-only fake function framework (v0.9.1)
RUN curl -fsSL https://github.com/meekrosoft/fff/releases/download/v1.1/fff.h \
    -o /usr/local/include/fff.h

# Update PATH
ENV PATH="/usr/local/Renesas/CC-RH/V2.08.00/bin:${PATH}"

RUN apt-get clean -y && \
    apt-get autoremove -y && \
    apt-get autoclean -y && \
    rm -rf /tmp/* && \
    rm -rf /var/lib/apt/lists/*

ARG USER_ID=1000
ARG GROUP_ID=1000
RUN groupadd -g ${GROUP_ID} build && \
    useradd -m -u ${USER_ID} -g ${GROUP_ID} build

CMD ["/bin/bash"]
