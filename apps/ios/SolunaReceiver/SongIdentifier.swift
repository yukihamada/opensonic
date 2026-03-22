//
//  SongIdentifier.swift
//  SolunaReceiver
//
//  Now Playing card — shows current track metadata from relay META broadcast.
//

import SwiftUI

struct NowPlayingCard: View {
    let title: String?
    let artist: String?

    var body: some View {
        if let title {
            HStack(spacing: 8) {
                Image(systemName: "music.note")
                    .font(.system(size: 11))
                    .foregroundColor(.solunaSol)
                VStack(alignment: .leading, spacing: 1) {
                    Text(title)
                        .font(.system(size: 12, weight: .bold))
                        .foregroundColor(.white)
                        .lineLimit(1)
                    if let artist {
                        Text(artist)
                            .font(.system(size: 10))
                            .foregroundColor(.white.opacity(0.5))
                            .lineLimit(1)
                    }
                }
                Spacer()
            }
            .padding(8)
            .background(Color.white.opacity(0.06))
            .clipShape(RoundedRectangle(cornerRadius: 8))
        }
    }
}
