FROM python:3.9

ENV RUNNER_ALLOW_RUNASROOT=1

# Configure bazel
RUN apt-get install -y apt-transport-https curl gnupg
RUN curl -fsSL https://bazel.build/bazel-release.pub.gpg | gpg --dearmor >bazel-archive-keyring.gpg
RUN mv bazel-archive-keyring.gpg /usr/share/keyrings
RUN echo "deb [arch=amd64 signed-by=/usr/share/keyrings/bazel-archive-keyring.gpg] https://storage.googleapis.com/bazel-apt stable jdk1.8" | tee /etc/apt/sources.list.d/bazel.list

# Install dependencies
RUN apt-get update
RUN apt-get install -y bazel
RUN apt-get install -y openjdk-11-jdk
RUN apt-get install -y clang
RUN apt-get install -y clang-format
RUN apt-get install -y clang-13
RUN apt-get install -y nodejs
RUN apt-get install -y jq

# Install GitHub action runner
RUN mkdir /actions-runner
WORKDIR /actions-runner
RUN curl -o actions-runner-linux-x64-2.298.2.tar.gz -L https://github.com/actions/runner/releases/download/v2.298.2/actions-runner-linux-x64-2.298.2.tar.gz
RUN echo "0bfd792196ce0ec6f1c65d2a9ad00215b2926ef2c416b8d97615265194477117  actions-runner-linux-x64-2.298.2.tar.gz" | shasum -a 256
RUN tar xzf ./actions-runner-linux-x64-2.298.2.tar.gz

COPY start.sh start.sh
RUN chmod +x start.sh

ENTRYPOINT ["./start.sh"]
