package com.soluna.receiver

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import com.soluna.receiver.ui.MainScreen

class MainActivity : ComponentActivity() {

    private val audioPlayer = AudioPlayer()
    private val solunaClient = SolunaClient(audioPlayer)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        setContent {
            MaterialTheme {
                Surface(color = MaterialTheme.colorScheme.background) {
                    MainScreen(client = solunaClient)
                }
            }
        }
    }

    override fun onDestroy() {
        solunaClient.disconnect()
        super.onDestroy()
    }
}
