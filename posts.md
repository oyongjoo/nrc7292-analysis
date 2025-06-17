---
layout: default
title: All Posts
---

# All Posts

{% for post in site.posts %}
## [{{ post.title }}]({{ post.url | relative_url }})

**{{ post.date | date: "%B %d, %Y" }}**

{% if post.category %}
**Category:** {{ post.category }}
{% endif %}

{% if post.tags %}
**Tags:** 
{% for tag in post.tags %}
<span class="tag">{{ tag }}</span>
{% endfor %}
{% endif %}

{% if post.excerpt %}
{{ post.excerpt }}
{% endif %}

---
{% endfor %}