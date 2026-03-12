class Soluna < Formula
  desc "Open Network Audio System — planet-scale audio streaming with P2P, copyright detection, and wallet"
  homepage "https://github.com/yukihamada/opensonic"
  url "https://github.com/yukihamada/opensonic/archive/refs/tags/v0.3.0.tar.gz"
  sha256 "PLACEHOLDER_SHA256"
  license "MIT"

  depends_on "cmake" => :build

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DSOLUNA_BUILD_TESTS=OFF",
           "-DSOLUNA_ENABLE_AES67=ON",
           *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build", "--prefix", prefix
  end

  test do
    assert_match "Soluna Daemon", shell_output("#{bin}/solunad --help 2>&1", 1)
  end

  service do
    run [opt_bin/"solunad", "--rx", "--device", "default", "--channels", "2"]
    keep_alive true
    log_path var/"log/soluna.log"
    error_log_path var/"log/soluna.log"
  end
end
