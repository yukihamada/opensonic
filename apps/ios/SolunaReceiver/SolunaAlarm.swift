//
//  SolunaAlarm.swift
//  Soluna
//
//  Alarm — wake up to Soluna radio on a chosen channel
//

import Foundation
import UserNotifications

@MainActor
class SolunaAlarmManager: ObservableObject {
    static let shared = SolunaAlarmManager()

    @Published var alarmTime: Date? = nil
    @Published var alarmChannel: String = "chill"
    @Published var isAlarmSet = false

    func setAlarm(time: Date, channel: String) {
        alarmTime = time
        alarmChannel = channel
        isAlarmSet = true

        // Schedule local notification
        let content = UNMutableNotificationContent()
        content.title = "Soluna Radio"
        content.body = "Time to wake up with \(channel.capitalized)!"
        content.sound = .default
        content.userInfo = ["channel": channel, "action": "alarm"]

        let calendar = Calendar.current
        let components = calendar.dateComponents([.hour, .minute], from: time)
        let trigger = UNCalendarNotificationTrigger(dateMatching: components, repeats: false)

        let request = UNNotificationRequest(identifier: "soluna-alarm", content: content, trigger: trigger)
        UNUserNotificationCenter.current().add(request)

        // Save to UserDefaults
        UserDefaults.standard.set(time.timeIntervalSince1970, forKey: "soluna_alarm_time")
        UserDefaults.standard.set(channel, forKey: "soluna_alarm_channel")
    }

    func cancelAlarm() {
        UNUserNotificationCenter.current().removePendingNotificationRequests(withIdentifiers: ["soluna-alarm"])
        isAlarmSet = false
        alarmTime = nil
        UserDefaults.standard.removeObject(forKey: "soluna_alarm_time")
    }

    func restoreAlarm() {
        let ts = UserDefaults.standard.double(forKey: "soluna_alarm_time")
        if ts > 0 {
            let time = Date(timeIntervalSince1970: ts)
            if time > Date() {
                alarmTime = time
                alarmChannel = UserDefaults.standard.string(forKey: "soluna_alarm_channel") ?? "chill"
                isAlarmSet = true
            }
        }
    }
}
