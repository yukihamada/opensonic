/**
 * Soluna — Auto Router Tests
 *
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <soluna/control/auto_router.h>

using namespace soluna::control;

class AutoRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        router = std::make_unique<AutoRouter>();
    }

    std::unique_ptr<AutoRouter> router;
};

TEST_F(AutoRouterTest, AddRule) {
    RouteRule rule;
    rule.name = "test-rule";
    rule.trigger = TriggerType::DeviceConnected;
    rule.pattern = "esp32-.*";

    router->add_rule(rule);

    auto rules = router->get_rules();
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].name, "test-rule");
}

TEST_F(AutoRouterTest, RemoveRule) {
    RouteRule rule;
    rule.name = "test-rule";
    router->add_rule(rule);

    EXPECT_TRUE(router->remove_rule("test-rule"));
    EXPECT_FALSE(router->remove_rule("nonexistent"));
    EXPECT_TRUE(router->get_rules().empty());
}

TEST_F(AutoRouterTest, ClearRules) {
    RouteRule rule1, rule2;
    rule1.name = "rule1";
    rule2.name = "rule2";
    router->add_rule(rule1);
    router->add_rule(rule2);

    router->clear_rules();
    EXPECT_TRUE(router->get_rules().empty());
}

TEST_F(AutoRouterTest, RulePriority) {
    RouteRule low, high;
    low.name = "low-priority";
    low.priority = 0;
    high.name = "high-priority";
    high.priority = 10;

    router->add_rule(low);
    router->add_rule(high);

    auto rules = router->get_rules();
    ASSERT_EQ(rules.size(), 2u);
    EXPECT_EQ(rules[0].name, "high-priority");
    EXPECT_EQ(rules[1].name, "low-priority");
}

TEST_F(AutoRouterTest, EnableDisableRule) {
    RouteRule rule;
    rule.name = "test-rule";
    rule.enabled = true;
    router->add_rule(rule);

    EXPECT_TRUE(router->set_rule_enabled("test-rule", false));
    EXPECT_FALSE(router->get_rules()[0].enabled);

    EXPECT_TRUE(router->set_rule_enabled("test-rule", true));
    EXPECT_TRUE(router->get_rules()[0].enabled);

    EXPECT_FALSE(router->set_rule_enabled("nonexistent", false));
}

TEST_F(AutoRouterTest, ProcessEventMatchesPattern) {
    RouteRule rule;
    rule.name = "esp32-connect";
    rule.trigger = TriggerType::DeviceConnected;
    rule.pattern = "esp32-.*";
    rule.enabled = true;

    RouteAction action;
    action.type = ActionType::AddRoute;
    action.source = "main:0";
    action.sink = "$device:0";
    rule.actions.push_back(action);

    router->add_rule(rule);

    int callback_count = 0;
    router->set_action_callback([&](const RouteAction&, const RouteEvent&) {
        callback_count++;
        return soluna::Result<void>{};
    });

    RouteEvent event;
    event.type = TriggerType::DeviceConnected;
    event.device_id = "esp32-001";
    event.device_name = "esp32-speaker";

    int matched = router->process_event(event);
    EXPECT_EQ(matched, 1);
    EXPECT_EQ(callback_count, 1);
}

TEST_F(AutoRouterTest, ProcessEventNoMatch) {
    RouteRule rule;
    rule.name = "esp32-connect";
    rule.trigger = TriggerType::DeviceConnected;
    rule.pattern = "esp32-.*";
    rule.enabled = true;

    router->add_rule(rule);

    int callback_count = 0;
    router->set_action_callback([&](const RouteAction&, const RouteEvent&) {
        callback_count++;
        return soluna::Result<void>{};
    });

    RouteEvent event;
    event.type = TriggerType::DeviceConnected;
    event.device_id = "raspi-001";
    event.device_name = "raspi-receiver";

    int matched = router->process_event(event);
    EXPECT_EQ(matched, 0);
    EXPECT_EQ(callback_count, 0);
}

TEST_F(AutoRouterTest, ProcessEventWrongTrigger) {
    RouteRule rule;
    rule.name = "connect-rule";
    rule.trigger = TriggerType::DeviceConnected;
    rule.pattern = ".*";
    rule.enabled = true;

    router->add_rule(rule);

    int callback_count = 0;
    router->set_action_callback([&](const RouteAction&, const RouteEvent&) {
        callback_count++;
        return soluna::Result<void>{};
    });

    RouteEvent event;
    event.type = TriggerType::DeviceDisconnected;  // Wrong type
    event.device_name = "test-device";

    int matched = router->process_event(event);
    EXPECT_EQ(matched, 0);
    EXPECT_EQ(callback_count, 0);
}

TEST_F(AutoRouterTest, DisabledRuleSkipped) {
    RouteRule rule;
    rule.name = "disabled-rule";
    rule.trigger = TriggerType::DeviceConnected;
    rule.pattern = ".*";
    rule.enabled = false;

    router->add_rule(rule);

    int callback_count = 0;
    router->set_action_callback([&](const RouteAction&, const RouteEvent&) {
        callback_count++;
        return soluna::Result<void>{};
    });

    RouteEvent event;
    event.type = TriggerType::DeviceConnected;
    event.device_name = "test-device";

    int matched = router->process_event(event);
    EXPECT_EQ(matched, 0);
    EXPECT_EQ(callback_count, 0);
}

TEST_F(AutoRouterTest, StopOnMatch) {
    RouteRule rule1, rule2;
    rule1.name = "rule1";
    rule1.trigger = TriggerType::DeviceConnected;
    rule1.pattern = ".*";
    rule1.stop_on_match = true;
    rule1.priority = 10;

    rule2.name = "rule2";
    rule2.trigger = TriggerType::DeviceConnected;
    rule2.pattern = ".*";
    rule2.priority = 0;

    router->add_rule(rule1);
    router->add_rule(rule2);

    int callback_count = 0;
    router->set_action_callback([&](const RouteAction&, const RouteEvent&) {
        callback_count++;
        return soluna::Result<void>{};
    });

    RouteEvent event;
    event.type = TriggerType::DeviceConnected;
    event.device_name = "test-device";

    int matched = router->process_event(event);
    EXPECT_EQ(matched, 1);  // Only first rule matched (stop_on_match)
}

TEST_F(AutoRouterTest, SubstituteVariables) {
    RouteEvent event;
    event.device_id = "esp32-001";
    event.device_name = "living-room-speaker";
    event.stream_id = "stream-42";
    event.variables["channel"] = "stereo";

    std::string pattern = "$device:$name/$stream/$channel";
    std::string result = AutoRouter::substitute_variables(pattern, event);

    EXPECT_EQ(result, "esp32-001:living-room-speaker/stream-42/stereo");
}

TEST_F(AutoRouterTest, GlobPatternMatching) {
    RouteRule rule;
    rule.name = "glob-rule";
    rule.trigger = TriggerType::DeviceConnected;
    rule.pattern = "esp32-speaker-*";  // Glob pattern (not regex)
    rule.enabled = true;

    router->add_rule(rule);

    int callback_count = 0;
    router->set_action_callback([&](const RouteAction&, const RouteEvent&) {
        callback_count++;
        return soluna::Result<void>{};
    });

    RouteEvent event;
    event.type = TriggerType::DeviceConnected;
    event.device_name = "esp32-speaker-living-room";

    int matched = router->process_event(event);
    EXPECT_EQ(matched, 1);
}

TEST_F(AutoRouterTest, EmptyPatternMatchesAll) {
    RouteRule rule;
    rule.name = "match-all";
    rule.trigger = TriggerType::DeviceConnected;
    rule.pattern = "";  // Empty matches all
    rule.enabled = true;

    router->add_rule(rule);

    int callback_count = 0;
    router->set_action_callback([&](const RouteAction&, const RouteEvent&) {
        callback_count++;
        return soluna::Result<void>{};
    });

    RouteEvent event;
    event.type = TriggerType::DeviceConnected;
    event.device_name = "any-device-name";

    int matched = router->process_event(event);
    EXPECT_EQ(matched, 1);
}

TEST_F(AutoRouterTest, MultipleActions) {
    RouteRule rule;
    rule.name = "multi-action";
    rule.trigger = TriggerType::DeviceConnected;
    rule.pattern = ".*";
    rule.enabled = true;

    RouteAction action1, action2, action3;
    action1.type = ActionType::AddRoute;
    action2.type = ActionType::SetGain;
    action3.type = ActionType::Unmute;
    rule.actions = {action1, action2, action3};

    router->add_rule(rule);

    std::vector<ActionType> executed_actions;
    router->set_action_callback([&](const RouteAction& action, const RouteEvent&) {
        executed_actions.push_back(action.type);
        return soluna::Result<void>{};
    });

    RouteEvent event;
    event.type = TriggerType::DeviceConnected;
    event.device_name = "test";

    router->process_event(event);

    ASSERT_EQ(executed_actions.size(), 3u);
    EXPECT_EQ(executed_actions[0], ActionType::AddRoute);
    EXPECT_EQ(executed_actions[1], ActionType::SetGain);
    EXPECT_EQ(executed_actions[2], ActionType::Unmute);
}

TEST_F(AutoRouterTest, TriggerTypeConversion) {
    EXPECT_EQ(parse_trigger_type("device_connected"), TriggerType::DeviceConnected);
    EXPECT_EQ(parse_trigger_type("device_disconnected"), TriggerType::DeviceDisconnected);
    EXPECT_EQ(parse_trigger_type("stream_created"), TriggerType::StreamCreated);
    EXPECT_EQ(parse_trigger_type("stream_ended"), TriggerType::StreamEnded);
    EXPECT_EQ(parse_trigger_type("manual"), TriggerType::Manual);
    EXPECT_EQ(parse_trigger_type("unknown"), TriggerType::Manual);

    EXPECT_STREQ(trigger_name(TriggerType::DeviceConnected), "device_connected");
}

TEST_F(AutoRouterTest, ActionTypeConversion) {
    EXPECT_EQ(parse_action_type("add_route"), ActionType::AddRoute);
    EXPECT_EQ(parse_action_type("remove_route"), ActionType::RemoveRoute);
    EXPECT_EQ(parse_action_type("set_gain"), ActionType::SetGain);
    EXPECT_EQ(parse_action_type("mute"), ActionType::Mute);
    EXPECT_EQ(parse_action_type("unmute"), ActionType::Unmute);
    EXPECT_EQ(parse_action_type("run_script"), ActionType::RunScript);
    EXPECT_EQ(parse_action_type("unknown"), ActionType::AddRoute);

    EXPECT_STREQ(action_name(ActionType::AddRoute), "add_route");
}

TEST_F(AutoRouterTest, LoadRulesFromYaml) {
    std::string yaml = R"(
auto_rules:
  - name: "connect-speakers"
    enabled: true
    priority: 10
    trigger:
      type: device_connected
      pattern: "esp32-speaker-*"
    actions:
      - type: add_route
        source: "main-out:0"
        sink: "$device:0"
        gain_db: -6.0
  - name: "low-priority-rule"
    priority: 0
    trigger:
      type: device_connected
      pattern: ".*"
    actions:
      - type: unmute
)";

    auto result = router->load_rules(yaml);
    EXPECT_TRUE(result.ok());

    auto rules = router->get_rules();
    ASSERT_EQ(rules.size(), 2u);

    // High priority first
    EXPECT_EQ(rules[0].name, "connect-speakers");
    EXPECT_EQ(rules[0].priority, 10);
    EXPECT_TRUE(rules[0].enabled);
    EXPECT_EQ(rules[0].trigger, TriggerType::DeviceConnected);
    EXPECT_EQ(rules[0].pattern, "esp32-speaker-*");
    ASSERT_EQ(rules[0].actions.size(), 1u);
    EXPECT_EQ(rules[0].actions[0].type, ActionType::AddRoute);
    EXPECT_EQ(rules[0].actions[0].source, "main-out:0");
    EXPECT_EQ(rules[0].actions[0].sink, "$device:0");
    EXPECT_FLOAT_EQ(rules[0].actions[0].gain_db, -6.0f);

    EXPECT_EQ(rules[1].name, "low-priority-rule");
    EXPECT_EQ(rules[1].priority, 0);
}
