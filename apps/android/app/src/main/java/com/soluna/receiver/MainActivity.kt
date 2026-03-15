package com.soluna.receiver

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.core.content.ContextCompat
import com.soluna.receiver.ui.MainScreen

class MainActivity : ComponentActivity() {

    private val audioPlayer = AudioPlayer()
    private val solunaClient = SolunaClient(audioPlayer)
    private val micTransmitter = MicTransmitter(solunaClient)

    private var pendingMicStart: (() -> Unit)? = null

    private val requestMicPermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) pendingMicStart?.invoke()
        pendingMicStart = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        setContent {
            MaterialTheme {
                Surface(color = MaterialTheme.colorScheme.background) {
                    MainScreen(
                        client = solunaClient,
                        micTransmitter = micTransmitter,
                        onRequestMicPermission = { onGranted ->
                            if (ContextCompat.checkSelfPermission(
                                    this, Manifest.permission.RECORD_AUDIO
                                ) == PackageManager.PERMISSION_GRANTED
                            ) {
                                onGranted()
                            } else {
                                pendingMicStart = onGranted
                                requestMicPermission.launch(Manifest.permission.RECORD_AUDIO)
                            }
                        }
                    )
                }
            }
        }
    }

    override fun onDestroy() {
        micTransmitter.stop()
        solunaClient.disconnect()
        super.onDestroy()
    }
}
