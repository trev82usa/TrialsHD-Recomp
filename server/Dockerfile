FROM python:3.12-slim
WORKDIR /app
COPY trials_server.py .
ENV TRIALS_DB=/data/trials.db TRIALS_CONFIG=/data/views.json TRIALS_PORT=8080 PYTHONUNBUFFERED=1
VOLUME /data
EXPOSE 8080
USER 99:100
HEALTHCHECK --interval=60s --timeout=5s CMD python3 -c "import urllib.request;urllib.request.urlopen('http://127.0.0.1:8080/health')"
CMD ["python3", "/app/trials_server.py"]
