FROM nginx:alpine

COPY web/ /usr/share/nginx/html/

# Custom nginx config: serve web/ files, SPA-friendly
COPY deploy/nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 8080
