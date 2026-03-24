Pod::Spec.new do |s|
  s.name             = 'SolunaSDK'
  s.version          = '1.0.0'
  s.summary          = 'Full-featured audio streaming SDK for iOS and macOS.'
  s.description      = <<-DESC
    SolunaSDK connects your app to the Soluna relay network, enabling
    real-time audio streaming, P2P peer discovery, DJ dual-deck mixing,
    multi-device sync, end-to-end encrypted audio, and SwiftUI integration.
    Built on the OSTP protocol (RTP-based) with zero external dependencies.
  DESC

  s.homepage         = 'https://github.com/yukihamada/opensonic'
  s.license          = { :type => 'Apache-2.0', :file => 'LICENSE' }
  s.author           = { 'Enabler DAO' => 'info@enablerdao.com' }
  s.source           = { :git => 'https://github.com/yukihamada/opensonic.git', :tag => "v#{s.version}" }

  s.swift_version    = '5.9'
  s.ios.deployment_target = '16.0'
  s.osx.deployment_target = '13.0'

  s.source_files     = 'sdk/swift/Sources/SolunaSDK/**/*.swift'

  s.frameworks       = 'AVFoundation', 'CryptoKit', 'MediaPlayer', 'Accelerate', 'Speech', 'StoreKit', 'Network'
  s.ios.frameworks   = 'MultipeerConnectivity', 'UIKit'
  s.osx.frameworks   = 'CoreAudio', 'AppKit'

  s.requires_arc     = true
end
