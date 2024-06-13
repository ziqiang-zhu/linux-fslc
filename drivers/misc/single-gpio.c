#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/gpio/consumer.h>

#define GPIO_ACTIVE 1
#define GPIO_DEACTIVE 0

struct single_gpio_device {
	const char *name;
	struct device_node *nd;

	struct timer_list timer;
	bool timer_state;
	int duration;

	struct timer_list deactivate_timer;

	struct gpio_desc *desc;
	int state;
};

static void gpio_periodic_handler(struct timer_list *timer)
{
	struct single_gpio_device *gpio = from_timer(gpio, timer, timer);

	gpio->state = !gpio->state;
	gpiod_set_value(gpio->desc, gpio->state);
	mod_timer(&gpio->timer, jiffies + msecs_to_jiffies(gpio->duration));
}

static ssize_t periodic_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct single_gpio_device *gpio = platform_get_drvdata(to_platform_device(dev));

	return sprintf(buf, "%s timer %s\n", gpio->name, gpio->timer_state ? "activated" : "deactivated");
}

static ssize_t periodic_state_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct single_gpio_device *gpio = platform_get_drvdata(to_platform_device(dev));

	if (!gpio->timer_state && sysfs_streq(buf, "on")) {
		gpio->timer_state = true;
		mod_timer(&gpio->timer, jiffies + msecs_to_jiffies(gpio->duration));
	} else if (gpio->timer_state && sysfs_streq(buf, "off")) {
		gpio->timer_state = false;
		timer_shutdown(&gpio->timer);
		gpio->state = GPIO_DEACTIVE;
		gpiod_set_value(gpio->desc, gpio->state);
	}

	pr_info("%s timer %s\n", gpio->name, gpio->timer_state ? "activated" : "deactivated");

	return count;
}
static DEVICE_ATTR(periodic_state, 0644, periodic_state_show, periodic_state_store);

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct single_gpio_device *gpio = platform_get_drvdata(to_platform_device(dev));

	return sprintf(buf, "%s %s\n", gpio->name, gpio->state ? "activated" : "deactivated");
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct single_gpio_device *gpio = platform_get_drvdata(to_platform_device(dev));

	if (gpio->timer_state)
		return count;

	if (sysfs_streq(buf, "on"))
		gpio->state = GPIO_ACTIVE;
	else if (sysfs_streq(buf, "off"))
		gpio->state = GPIO_DEACTIVE;

	gpiod_set_value(gpio->desc, gpio->state);
	pr_info("%s %s\n", gpio->name, gpio->state ? "activated" : "deactivated");

	return count;
}
static DEVICE_ATTR(state, 0644, state_show, state_store);

static void deactivate_handler(struct timer_list *timer)
{
	struct single_gpio_device *gpio = from_timer(gpio, timer, deactivate_timer);

	gpiod_set_value(gpio->desc, GPIO_DEACTIVE);
	gpio->state = GPIO_DEACTIVE;
}

static ssize_t activate_brief_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct single_gpio_device *gpio = platform_get_drvdata(to_platform_device(dev));

	return sprintf(buf, "%s %s\n", gpio->name, gpio->state ? "activated" : "deactivated");
}

static ssize_t activate_brief_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct single_gpio_device *gpio = platform_get_drvdata(to_platform_device(dev));

	if (gpio->timer_state)
		return count;

	if (gpio->state != GPIO_ACTIVE && sysfs_streq(buf, "on")) {
		gpio->state = GPIO_ACTIVE;
		gpiod_set_value(gpio->desc, gpio->state);
		mod_timer(&gpio->deactivate_timer, jiffies + msecs_to_jiffies(gpio->duration));
	}

	return count;
}
static DEVICE_ATTR(activate_brief, 0644, activate_brief_show, activate_brief_store);

static struct of_device_id gpio_device_of_match[] = {
	{ .compatible = "ziq,single-gpio", .data = NULL, },
	{ }
};

static int gpio_device_probe(struct platform_device *pdev)
{
	int ret;
	struct single_gpio_device *gpio;

	gpio = kzalloc(sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->nd = pdev->dev.of_node;
	gpio->name = dev_name(&pdev->dev);
	gpio->timer_state = false;
	gpio->state = GPIO_DEACTIVE;
	timer_setup(&gpio->timer, gpio_periodic_handler, 0);
	timer_setup(&gpio->deactivate_timer, deactivate_handler, 0);

	gpio->desc = gpiod_get(&pdev->dev, "single", GPIOD_OUT_LOW);
	if (IS_ERR(gpio->desc)) {
		pr_err("get led gpio fail.\n");
		goto exit;
	}

	ret = of_property_read_u32(gpio->nd, "periodic-duration", &gpio->duration);
	if (ret || gpio->duration < 300 || gpio->duration > 2000) {
		pr_warn("warn, duration may not right, force 300 mesc\n");
		gpio->duration = 300;
	}

	if (device_property_present(&pdev->dev, "ziq,default-timer-activate")) {
		gpio->timer_state = true;
		mod_timer(&gpio->timer, jiffies + msecs_to_jiffies(gpio->duration));
	}

	platform_set_drvdata(pdev, gpio);

	device_create_file(&pdev->dev, &dev_attr_state);
	device_create_file(&pdev->dev, &dev_attr_periodic_state);
	device_create_file(&pdev->dev, &dev_attr_activate_brief);

	return 0;
exit:
	kfree(gpio);
	return ret;
}

static int gpio_device_remove(struct platform_device *pdev)
{
	struct single_gpio_device *gpio = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_state);
	device_remove_file(&pdev->dev, &dev_attr_periodic_state);
	device_remove_file(&pdev->dev, &dev_attr_activate_brief);

	timer_delete(&gpio->timer);
	timer_delete(&gpio->deactivate_timer);
	gpiod_put(gpio->desc);
	kfree(gpio);

	return 0;
}

static struct platform_driver gpio_platform_driver = {
	.driver = {
		.name = "single-gpio",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(gpio_device_of_match),
	},
	.probe = gpio_device_probe,
	.remove = gpio_device_remove,
};

static int __init gpio_platform_driver_init(void)
{
	return platform_driver_register(&gpio_platform_driver);
}

static void __exit gpio_platform_driver_exit(void)
{
	platform_driver_unregister(&gpio_platform_driver);
}

module_init(gpio_platform_driver_init);
module_exit(gpio_platform_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ziqiang.zhu");
